/*
 * Copyright (C) 2018-2020 Canonical, Ltd.
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

#include "sftp_server_test_fixture.h"
#include "signal.h"

#include <multipass/exceptions/sshfs_missing_error.h>
#include <multipass/ssh/ssh_session.h>
#include <multipass/sshfs_mount/sshfs_mount.h>

#include <gmock/gmock.h>

namespace mp = multipass;
namespace mpt = multipass::test;

using namespace testing;

namespace
{
struct SshfsMount : public mp::test::SftpServerTest
{
    SshfsMount()
    {
        channel_read.returnValue(0);
        channel_is_closed.returnValue(0);
    }

    mp::SshfsMount make_sshfsmount()
    {
        mp::SSHSession session{"a", 42};
        return {std::move(session), default_source, default_target, default_map, default_map};
    }

    auto make_exec_that_fails_for(const std::vector<std::string>& expected_cmds, bool& invoked)
    {
        auto request_exec = [this, expected_cmds, &invoked](ssh_channel, const char* raw_cmd) {
            std::string cmd{raw_cmd};
            for (const auto expected_cmd : expected_cmds)
            {
                if (cmd.find(expected_cmd) != std::string::npos)
                {
                    invoked = true;
                    exit_status_mock.return_exit_code(SSH_ERROR);
                }
            }
            return SSH_OK;
        };
        return request_exec;
    }

    auto make_channel_read_return(const std::string& output, std::string::size_type& remaining, bool& prereq_invoked)
    {
        auto channel_read = [&output, &remaining, &prereq_invoked](ssh_channel, void* dest, uint32_t count,
                                                                   int is_stderr, int) {
            if (!prereq_invoked)
                return 0u;
            const auto num_to_copy = std::min(count, static_cast<uint32_t>(remaining));
            const auto begin = output.begin() + output.size() - remaining;
            std::copy_n(begin, num_to_copy, reinterpret_cast<char*>(dest));
            remaining -= num_to_copy;
            return num_to_copy;
        };
        return channel_read;
    }

    mpt::ExitStatusMock exit_status_mock;
    decltype(MOCK(ssh_channel_read_timeout)) channel_read{MOCK(ssh_channel_read_timeout)};
    decltype(MOCK(ssh_channel_is_closed)) channel_is_closed{MOCK(ssh_channel_is_closed)};

    std::string default_source{"source"};
    std::string default_target{"target"};
    std::unordered_map<int, int> default_map;
    int default_id{1000};
};
} // namespace

TEST_F(SshfsMount, throws_when_sshfs_does_not_exist)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"sudo multipass-sshfs.env", "which sshfs"}, invoked);
    REPLACE(ssh_channel_request_exec, request_exec);

    EXPECT_THROW(make_sshfsmount(), mp::SSHFSMissingError);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, throws_when_unable_to_make_target_dir)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"mkdir"}, invoked);
    REPLACE(ssh_channel_request_exec, request_exec);

    EXPECT_THROW(make_sshfsmount(), std::runtime_error);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, throws_when_unable_to_obtain_user_id_name)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"id -nu"}, invoked);

    REPLACE(ssh_channel_request_exec, request_exec);

    EXPECT_THROW(make_sshfsmount(), std::runtime_error);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, throws_when_unable_to_obtain_group_id_name)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"id -ng"}, invoked);

    REPLACE(ssh_channel_request_exec, request_exec);

    EXPECT_THROW(make_sshfsmount(), std::runtime_error);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, throws_when_unable_to_chown)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"chown"}, invoked);

    REPLACE(ssh_channel_request_exec, request_exec);

    EXPECT_THROW(make_sshfsmount(), std::runtime_error);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, throws_when_unable_to_obtain_uid)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"id -u"}, invoked);

    REPLACE(ssh_channel_request_exec, request_exec);

    EXPECT_THROW(make_sshfsmount(), std::runtime_error);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, throws_when_uid_is_not_an_integer)
{
    bool invoked{false};
    auto request_exec = [&invoked](ssh_channel, const char* raw_cmd) {
        std::string cmd{raw_cmd};
        if (cmd.find("id -u") != std::string::npos)
        {
            invoked = true;
        }
        return SSH_OK;
    };
    REPLACE(ssh_channel_request_exec, request_exec);

    std::string output{"ubuntu"};
    auto remaining = output.size();
    auto channel_read = make_channel_read_return(output, remaining, invoked);
    REPLACE(ssh_channel_read_timeout, channel_read);

    EXPECT_THROW(make_sshfsmount(), std::invalid_argument);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, throws_when_unable_to_obtain_gid)
{
    bool uid_invoked{false};
    bool gid_invoked{false};
    auto request_exec = [this, &uid_invoked, &gid_invoked](ssh_channel, const char* raw_cmd) {
        std::string cmd{raw_cmd};
        if (cmd.find("id -u") != std::string::npos)
        {
            uid_invoked = true;
        }
        else if (cmd.find("id -g") != std::string::npos)
        {
            uid_invoked = false;
            gid_invoked = true;
            exit_status_mock.return_exit_code(SSH_ERROR);
        }
        return SSH_OK;
    };
    REPLACE(ssh_channel_request_exec, request_exec);

    std::string output{"1000"};
    auto remaining = output.size();
    auto channel_read = make_channel_read_return(output, remaining, uid_invoked);
    REPLACE(ssh_channel_read_timeout, channel_read);

    EXPECT_THROW(make_sshfsmount(), std::runtime_error);
    EXPECT_TRUE(gid_invoked);
}

TEST_F(SshfsMount, unblocks_when_sftpserver_exits)
{
    bool invoked{false};
    std::string output{"1000"};
    auto remaining = output.size();
    auto channel_read = make_channel_read_return(output, remaining, invoked);
    REPLACE(ssh_channel_read_timeout, channel_read);

    auto request_exec = [&invoked, &remaining, &output](ssh_channel, const char* raw_cmd) {
        std::string cmd{raw_cmd};
        if (cmd.find("id -u") != std::string::npos)
        {
            invoked = true;
        }
        else if (cmd.find("id -g") != std::string::npos)
        {
            // Reset for the next channel read
            remaining = output.size();
        }
        return SSH_OK;
    };
    REPLACE(ssh_channel_request_exec, request_exec);

    mpt::Signal client_message;
    auto get_client_msg = [&client_message](sftp_session) {
        client_message.wait();
        return nullptr;
    };
    REPLACE(sftp_get_client_message, get_client_msg);

    bool stopped_ok = false;
    std::thread mount([&] {
        auto sshfs_mount = make_sshfsmount(); // blocks until it asked to quit
        stopped_ok = true;
    });

    client_message.signal();

    mount.join();
    EXPECT_TRUE(stopped_ok);
}

TEST_F(SshfsMount, invalid_fuse_version_throws)
{
    bool invoked{false};
    std::string output;
    auto remaining = output.size();
    auto channel_read = make_channel_read_return(output, remaining, invoked);
    REPLACE(ssh_channel_read_timeout, channel_read);

    auto request_exec = [&invoked, &remaining, &output](ssh_channel, const char* raw_cmd) {
        std::string cmd{raw_cmd};
        if (cmd.find("snap run multipass-sshfs.env") != std::string::npos)
        {
            output = "LD_LIBRARY_PATH=/foo/bar\nSNAP=/baz\n";
            remaining = output.size();
            invoked = true;
        }
        else if (cmd.find("sshfs -V") != std::string::npos)
        {
            output = "FUSE library version: fu.man.chu";
            remaining = output.size();
        }
        return SSH_OK;
    };
    REPLACE(ssh_channel_request_exec, request_exec);

    EXPECT_THROW(make_sshfsmount(), std::runtime_error);
    EXPECT_TRUE(invoked);
}
