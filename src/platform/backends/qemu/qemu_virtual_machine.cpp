/*
 * Copyright (C) 2017-2019 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu_virtual_machine.h"

#include "dnsmasq_server.h"
#include "qemu_vm_process_spec.h"
#include <shared/linux/backend_utils.h>
#include <shared/linux/process_factory.h>

#include <multipass/exceptions/start_exception.h>
#include <multipass/logging/log.h>
#include <multipass/process.h>
#include <multipass/ssh/ssh_session.h>
#include <multipass/utils.h>
#include <multipass/vm_status_monitor.h>

#include <multipass/format.h>

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QSysInfo>
#include <QTemporaryFile>

#include <thread>

namespace mp = multipass;
namespace mpl = multipass::logging;

namespace
{
// Qemu command version
constexpr int latest_vm_command_version = 1;
/*
 * Qemu will often fail to resume a VM that was run with a different command line.
 * To keep backward compatibility, we version each Qemu command line iteration and save the
 * version used in the metadata
 *
 * Versions:
 *  1 - changed how cloud-init ISO was specified:
 *      Replaced: "-drive file=cloud-init.iso,if=virtio,format=raw,snapshot=off,read-only"
 *      With:     "-cdrom cloud-init.iso"
 *      Note this was originally encompassed in the metadata as "use_cdrom" being true.
 *  0 - original
 */

constexpr auto suspend_tag = "suspend";
constexpr auto default_machine_type = "pc-i440fx-xenial";
constexpr auto vm_command_version_key = "vm_command_version";
constexpr auto machine_type_key = "machine_type";

int get_vm_command_version(const QJsonObject& metadata)
{
    int version;
    if (metadata.contains(vm_command_version_key))
    {
        version = metadata[vm_command_version_key].toInt();
    }
    else if (metadata.contains("use_cdrom") && metadata["use_cdrom"].toBool())
    {
        version = 1; // am retro-setting the "use_cdrom" metadata flag as version 1
    }
    else
    {
        version = 0; // fallback to original qemu command
    }
    return version;
}

void remove_tap_device(const QString& tap_device_name)
{
    if (mp::utils::run_cmd_for_status("ip", {"addr", "show", tap_device_name}))
    {
        mp::utils::run_cmd_for_status("ip", {"link", "delete", tap_device_name});
    }
}

auto qmp_execute_json(const QString& cmd)
{
    QJsonObject qmp;
    qmp.insert("execute", cmd);
    return QJsonDocument(qmp).toJson();
}

auto hmc_to_qmp_json(const QString& command_line)
{
    auto qmp = QJsonDocument::fromJson(qmp_execute_json("human-monitor-command")).object();

    QJsonObject cmd_line;
    cmd_line.insert("command-line", command_line);

    qmp.insert("arguments", cmd_line);

    return QJsonDocument(qmp).toJson();
}

bool instance_image_has_snapshot(const mp::Path& image_path)
{
    auto output = QString::fromStdString(
                      mp::utils::run_cmd_for_output(QStringLiteral("qemu-img"),
                                                    {QStringLiteral("snapshot"), QStringLiteral("-l"), image_path}))
                      .split('\n');

    for (const auto& line : output)
    {
        if (line.contains(suspend_tag))
        {
            return true;
        }
    }

    return false;
}

auto get_qemu_machine_type()
{
    QTemporaryFile dump_file;
    if (!dump_file.open())
    {
        return QString();
    }

    QProcess process;
    process.setProgram("qemu-system-" + mp::backend::cpu_arch());
    process.setArguments({"-nographic", "-dump-vmstate", dump_file.fileName()});
    process.start();
    process.waitForFinished();

    auto vmstate = QJsonDocument::fromJson(dump_file.readAll()).object();

    auto machine_type = vmstate["vmschkmachine"].toObject()["Name"].toString();
    return machine_type;
}

auto generate_metadata()
{
    QJsonObject metadata;

    metadata[machine_type_key] = get_qemu_machine_type();
    metadata[vm_command_version_key] = latest_vm_command_version;

    return metadata;
}
} // namespace

mp::QemuVirtualMachine::QemuVirtualMachine(const ProcessFactory* process_factory, const VirtualMachineDescription& desc,
                                           const std::string& tap_device_name, DNSMasqServer& dnsmasq_server,
                                           VMStatusMonitor& monitor)
    : VirtualMachine{instance_image_has_snapshot(desc.image.image_path) ? State::suspended : State::off, desc.vm_name},
      process_factory{process_factory},
      vm_desc{desc},
      tap_device_name{tap_device_name},
      mac_addr{desc.mac_addr},
      username{desc.ssh_username},
      dnsmasq_server{&dnsmasq_server},
      monitor{&monitor}
{
    if (!QFile::exists(desc.image.image_path) || !QFile::exists(desc.cloud_init_iso))
    {
        throw std::runtime_error("cannot start VM without an image");
    }
}

mp::QemuVirtualMachine::~QemuVirtualMachine()
{
    update_shutdown_status = false;
    if (state == State::running)
        suspend();
    else
        shutdown();

    remove_tap_device(QString::fromStdString(tap_device_name));

    if (vm_process)
        vm_process->wait_for_finished();
}

void mp::QemuVirtualMachine::start()
{
    int vm_command_version = latest_vm_command_version;

    if (state == State::running)
        return;

    if (state == State::suspending)
        throw std::runtime_error("cannot start the instance while suspending");

    QStringList extra_args;
    if (state == State::suspended)
    {
        mpl::log(mpl::Level::info, vm_name, fmt::format("Resuming from a suspended state"));

        auto metadata = monitor->retrieve_metadata_for(vm_name);

        vm_command_version = get_vm_command_version(metadata);

        auto machine_type = metadata[machine_type_key].toString();
        if (machine_type.isNull())
        {
            mpl::log(mpl::Level::info, vm_name,
                     fmt::format("Cannot determine QEMU machine type. Defaulting to '{}'.", default_machine_type));
            machine_type = default_machine_type;
        }

        mpl::log(mpl::Level::info, vm_name, fmt::format("Resuming from a suspended state"));
        extra_args << "-loadvm" << suspend_tag;
        extra_args << "-machine" << machine_type;

        if (metadata["use_cdrom"].toBool())
        {
            extra_args << "-cdrom" << cloud_init_path;
        }
        else
        {
            extra_args << "-drive"
                       << QString("file=%1,if=virtio,format=raw,snapshot=off,read-only").arg(cloud_init_path);
        }

        update_shutdown_status = true;
        delete_memory_snapshot = true;
    }
    else
    {
        monitor->update_metadata_for(vm_name, generate_metadata());
    }

    auto process_spec = std::make_unique<mp::QemuVMProcessSpec>(
        vm_desc, vm_command_version, QString::fromStdString(tap_device_name), QString::fromStdString(mac_addr));
    vm_process = process_factory->create_process(std::move(process_spec));
    create_connections();

    mpl::log(mpl::Level::debug, vm_name, fmt::format("process working dir '{}'", vm_process->working_directory()));
    mpl::log(mpl::Level::info, vm_name, fmt::format("process program '{}'", vm_process->program()));
    mpl::log(mpl::Level::info, vm_name, fmt::format("process arguments '{}'", vm_process->arguments().join(", ")));

    vm_process->start(extra_args);
    auto started = vm_process->wait_for_started();

    if (!started)
        throw std::runtime_error("failed to start qemu instance");

    vm_process->write(qmp_execute_json("qmp_capabilities"));
}

void mp::QemuVirtualMachine::stop()
{
    shutdown();
}

void mp::QemuVirtualMachine::shutdown()
{
    if (state == State::suspended)
    {
        mpl::log(mpl::Level::info, vm_name, fmt::format("Ignoring shutdown issued while suspended"));
    }
    else if ((state == State::running || state == State::delayed_shutdown || state == State::unknown) &&
             vm_process->running())
    {
        vm_process->write(qmp_execute_json("system_powerdown"));
        vm_process->wait_for_finished();
    }
    else
    {
        if (state == State::starting)
            update_shutdown_status = false;

        vm_process->kill();
        vm_process->wait_for_finished();
    }
}

void mp::QemuVirtualMachine::suspend()
{
    if ((state == State::running || state == State::delayed_shutdown) && vm_process->running())
    {
        vm_process->write(hmc_to_qmp_json("savevm " + QString::fromStdString(suspend_tag)));

        if (update_shutdown_status)
        {
            state = State::suspending;
            update_state();

            update_shutdown_status = false;
            vm_process->wait_for_finished();
        }
    }
    else if (state == State::off || state == State::suspended)
    {
        mpl::log(mpl::Level::info, vm_name, fmt::format("Ignoring suspend issued while stopped/suspended"));
        monitor->on_suspend();
    }
}

mp::VirtualMachine::State mp::QemuVirtualMachine::current_state()
{
    return state;
}

int mp::QemuVirtualMachine::ssh_port()
{
    return 22;
}

void mp::QemuVirtualMachine::update_state()
{
    monitor->persist_state_for(vm_name, state);
}

void mp::QemuVirtualMachine::on_started()
{
    state = State::starting;
    update_state();
    monitor->on_resume();
}

void mp::QemuVirtualMachine::on_error()
{
    state = State::off;
    update_state();
}

void mp::QemuVirtualMachine::on_shutdown()
{
    std::unique_lock<decltype(state_mutex)> lock{state_mutex};
    if (state == State::starting)
    {
        printf("EH??\n");
        saved_error_msg = fmt::format("{}: shutdown called while starting", vm_name);
        state_wait.wait(lock, [this] { return state == State::off; });
    }
    else
    {
        state = State::off;
    }

    ip = nullopt;
    update_state();
    lock.unlock();
    monitor->on_shutdown();
}

void mp::QemuVirtualMachine::on_suspend()
{
    state = State::suspended;
    monitor->on_suspend();
}

void mp::QemuVirtualMachine::on_restart()
{
    state = State::restarting;
    update_state();

    ip = nullopt;

    monitor->on_restart(vm_name);
}

void mp::QemuVirtualMachine::ensure_vm_is_running()
{
    std::lock_guard<decltype(state_mutex)> lock{state_mutex};
    if (!vm_process->running())
    {
        // Have to set 'off' here so there is an actual state change to compare to for
        // the cond var's predicate
        state = State::off;
        state_wait.notify_all();
        throw mp::StartException(vm_name, saved_error_msg);
    }
}

std::string mp::QemuVirtualMachine::ssh_hostname()
{
    if (!ip)
    {
        auto action = [this] {
            ensure_vm_is_running();
            auto result = dnsmasq_server->get_ip_for(mac_addr);
            if (result)
            {
                ip.emplace(result.value());
                return mp::utils::TimeoutAction::done;
            }
            else
            {
                return mp::utils::TimeoutAction::retry;
            }
        };
        auto on_timeout = [] { return std::runtime_error("failed to determine IP address"); };
        mp::utils::try_action_for(on_timeout, std::chrono::minutes(2), action);
    }

    return ip.value().as_string();
}

std::string mp::QemuVirtualMachine::ssh_username()
{
    return username;
}

std::string mp::QemuVirtualMachine::ipv4()
{
    if (!ip)
    {
        auto result = dnsmasq_server->get_ip_for(mac_addr);
        if (result)
            ip.emplace(result.value());
        else
            return "UNKNOWN";
    }

    return ip.value().as_string();
}

std::string mp::QemuVirtualMachine::ipv6()
{
    return {};
}

void mp::QemuVirtualMachine::wait_until_ssh_up(std::chrono::milliseconds timeout)
{
    mp::utils::wait_until_ssh_up(this, timeout, std::bind(&QemuVirtualMachine::ensure_vm_is_running, this));

    if (delete_memory_snapshot)
    {
        mpl::log(mpl::Level::debug, vm_name, fmt::format("Deleted memory snapshot"));
        vm_process->write(hmc_to_qmp_json("delvm " + QString::fromStdString(suspend_tag)));
        delete_memory_snapshot = false;
    }
}

void mp::QemuVirtualMachine::create_connections()
{
    QObject::connect(vm_process.get(), &Process::started, [this]() {
        mpl::log(mpl::Level::info, vm_name, "process started");
        on_started();
    });
    QObject::connect(vm_process.get(), &Process::ready_read_standard_output, [this]() {
        auto qmp_output = vm_process->read_all_standard_output();
        mpl::log(mpl::Level::debug, vm_name, fmt::format("QMP: {}", qmp_output));
        auto qmp_object = QJsonDocument::fromJson(qmp_output.split('\n').first()).object();
        auto event = qmp_object["event"];

        if (!event.isNull())
        {
            if (event.toString() == "RESET" && state != State::restarting)
            {
                mpl::log(mpl::Level::info, vm_name, "VM restarting");
                on_restart();
            }
            else if (event.toString() == "POWERDOWN")
            {
                mpl::log(mpl::Level::info, vm_name, "VM powering down");
            }
            else if (event.toString() == "SHUTDOWN")
            {
                mpl::log(mpl::Level::info, vm_name, "VM shut down");
            }
            else if (event.toString() == "STOP")
            {
                mpl::log(mpl::Level::info, vm_name, "VM suspending");
            }
            else if (event.toString() == "RESUME")
            {
                mpl::log(mpl::Level::info, vm_name, "VM suspended");
                if (state == State::suspending || state == State::running)
                {
                    vm_process->kill();
                    on_suspend();
                }
            }
        }
    });

    QObject::connect(vm_process.get(), &Process::ready_read_standard_error, [this]() {
        saved_error_msg = vm_process->read_all_standard_error().data();
        mpl::log(mpl::Level::warning, vm_name, saved_error_msg);
    });

    QObject::connect(vm_process.get(), &Process::state_changed, [this](QProcess::ProcessState newState) {
        mpl::log(mpl::Level::info, vm_name,
                 fmt::format("process state changed to {}", utils::qenum_to_string(newState)));
    });

    QObject::connect(vm_process.get(), &Process::error_occurred, [this](QProcess::ProcessError error) {
        // We just kill the process when suspending, so we don't want to print
        // out any scary error messages for this state
        if (update_shutdown_status)
        {
            mpl::log(mpl::Level::error, vm_name,
                     fmt::format("process error occurred {}", utils::qenum_to_string(error)));
            on_error();
        }
    });

    QObject::connect(vm_process.get(), &Process::finished, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        mpl::log(mpl::Level::info, vm_name,
                 fmt::format("process finished with exit code {} ({})", exitCode, utils::qenum_to_string(exitStatus)));
        if (update_shutdown_status || state == State::starting)
        {
            on_shutdown();
        }
    });
}
