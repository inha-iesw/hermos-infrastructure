#include "hermos/bridge/hermos_bridge_uds.hpp"
#include "hermos/bridge/hermos_bridge_ipc_event_loop_base.hpp"

#include <rclcpp/logger.hpp>
#include <rclcpp/logging.hpp>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

namespace
{

// Abstract-namespace address unique to this process + test-provided suffix, so
// parallel test binaries and repeated runs never collide with a live socket.
std::string make_unique_abstract_addr(const std::string & tag)
{
  std::string addr;
  addr.push_back('\0');
  addr += "hermos_bridge_uds_test_";
  addr += std::to_string(getpid());
  addr += "_";
  addr += tag;
  return addr;
}

// RAII owner so a failed EXPECT doesn't leak the fd into later tests.
class ScopedFd
{
public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd()
  {
    if (fd_ != -1) {
      close(fd_);
    }
  }
  ScopedFd(const ScopedFd &) = delete;
  ScopedFd & operator=(const ScopedFd &) = delete;
  int get() const { return fd_; }

private:
  int fd_;
};

}  // namespace

// ---- detail::fill_abstract_sockaddr ---------------------------------------

TEST(BridgeUdsAddrTest, EmptyAddressRejected)
{
  sockaddr_un sa{};
  EXPECT_THROW(hermos::detail::fill_abstract_sockaddr("", sa), std::invalid_argument);
}

TEST(BridgeUdsAddrTest, NonNulPrefixedAddressRejected)
{
  sockaddr_un sa{};
  EXPECT_THROW(
    hermos::detail::fill_abstract_sockaddr("hermos_bridge_no_nul", sa), std::invalid_argument);
}

TEST(BridgeUdsAddrTest, OverlongAddressRejected)
{
  sockaddr_un sa{};
  // Leading NUL + (sizeof(sun_path)) bytes = one byte too many for sun_path.
  std::string too_long(sizeof(sa.sun_path) + 1, 'x');
  too_long[0] = '\0';
  EXPECT_THROW(hermos::detail::fill_abstract_sockaddr(too_long, sa), std::length_error);
}

TEST(BridgeUdsAddrTest, ValidAddressPopulatesSockaddr)
{
  const std::string addr = std::string("\0hello", 6);  // NUL + "hello"

  sockaddr_un sa{};
  // Poison the padding to prove fill_abstract_sockaddr zeroes the whole struct.
  std::memset(&sa, 0xAB, sizeof(sa));

  const socklen_t alen = hermos::detail::fill_abstract_sockaddr(addr, sa);

  EXPECT_EQ(sa.sun_family, AF_UNIX);
  EXPECT_EQ(alen, static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + addr.size()));
  EXPECT_EQ(std::memcmp(sa.sun_path, addr.data(), addr.size()), 0);
  // Bytes past the address must remain zero (i.e. no trailing NUL is required
  // and no leftover poison bits leak to the kernel).
  for (size_t i = addr.size(); i < sizeof(sa.sun_path); ++i) {
    EXPECT_EQ(static_cast<unsigned char>(sa.sun_path[i]), 0u) << "byte " << i;
  }
}

TEST(BridgeUdsAddrTest, MaxLengthAddressAccepted)
{
  sockaddr_un probe{};
  // Boundary: exactly sizeof(sun_path) bytes (leading NUL + sizeof-1 name bytes).
  std::string boundary(sizeof(probe.sun_path), 'y');
  boundary[0] = '\0';

  sockaddr_un sa{};
  const socklen_t alen = hermos::detail::fill_abstract_sockaddr(boundary, sa);
  EXPECT_EQ(alen, static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + boundary.size()));
  EXPECT_EQ(std::memcmp(sa.sun_path, boundary.data(), boundary.size()), 0);
}

// ---- create_bridge_uds_listener -------------------------------------------

TEST(BridgeUdsListenerTest, RejectsInvalidAddress)
{
  EXPECT_THROW(hermos::create_bridge_uds_listener(""), std::invalid_argument);
  EXPECT_THROW(hermos::create_bridge_uds_listener("no-leading-nul"), std::invalid_argument);
}

TEST(BridgeUdsListenerTest, DuplicateBindFailsWithAddrInUse)
{
  const std::string addr = make_unique_abstract_addr("dup");
  ScopedFd first(hermos::create_bridge_uds_listener(addr));
  ASSERT_NE(first.get(), -1);

  try {
    ScopedFd second(hermos::create_bridge_uds_listener(addr));
    FAIL() << "second listener on the same abstract address must fail";
  } catch (const std::system_error & e) {
    EXPECT_EQ(e.code().value(), EADDRINUSE)
      << "expected EADDRINUSE, got " << e.code().value() << " (" << e.what() << ")";
  }
}

// ---- send_bridge_uds_message ----------------------------------------------

TEST(BridgeUdsSendTest, RoundTripDeliversPayload)
{
  const std::string addr = make_unique_abstract_addr("roundtrip");
  ScopedFd listener(hermos::create_bridge_uds_listener(addr));
  ASSERT_NE(listener.get(), -1);

  const std::array<std::uint8_t, 4> payload = {0xDE, 0xAD, 0xBE, 0xEF};
  const auto logger = rclcpp::get_logger("BridgeUdsSendTest");
  EXPECT_TRUE(hermos::send_bridge_uds_message(addr, payload.data(), payload.size(), logger));

  std::array<std::uint8_t, 32> buf{};
  const ssize_t n = recv(listener.get(), buf.data(), buf.size(), 0);
  ASSERT_EQ(n, static_cast<ssize_t>(payload.size()))
    << "recv failed or short read: " << (n < 0 ? strerror(errno) : "size mismatch");
  EXPECT_EQ(std::memcmp(buf.data(), payload.data(), payload.size()), 0);
}

TEST(BridgeUdsSendTest, InvalidAddressFailsFast)
{
  const auto logger = rclcpp::get_logger("BridgeUdsSendTest");
  const std::uint8_t byte = 0;
  // Empty and non-NUL-prefixed addresses are rejected inside
  // send_bridge_uds_message before any sendto()/retry loop runs, so this must
  // return false immediately rather than spending
  // BRIDGE_UDS_SEND_MAX_RETRIES * BRIDGE_UDS_SEND_RETRY_INTERVAL_US retrying.
  EXPECT_FALSE(hermos::send_bridge_uds_message("", &byte, sizeof(byte), logger));
  EXPECT_FALSE(hermos::send_bridge_uds_message("no-nul", &byte, sizeof(byte), logger));
}


TEST(BridgeUdsListenerTest, RequestsKernelCredentials)
{
  ScopedFd listener(hermos::create_bridge_uds_listener(make_unique_abstract_addr("credentials")));
  int enabled = 0;
  socklen_t size = sizeof(enabled);
  ASSERT_EQ(getsockopt(listener.get(), SOL_SOCKET, SO_PASSCRED, &enabled, &size), 0);
  EXPECT_EQ(enabled, 1);
}

TEST(BridgeUdsReceiveTest, AcceptedLocalDatagramReachesHandler)
{
  const auto address = make_unique_abstract_addr("authorized");
  const auto logger = rclcpp::get_logger("BridgeUdsReceiveTest");
  hermos::IpcEventLoopBase loop(logger, address, 4, {}, {});
  std::string received;
  loop.set_message_handler([&](const void * data, size_t size) {
    received.assign(static_cast<const char *>(data), size);
  });
  ASSERT_TRUE(hermos::send_bridge_uds_message(address, "test", 4, logger));
  ASSERT_TRUE(loop.spin_once(100));
  EXPECT_EQ(received, "test");
}

TEST(BridgeUdsReceiveTest, OversizedDatagramIsDroppedBeforeHandler)
{
  const auto address = make_unique_abstract_addr("oversized");
  const auto logger = rclcpp::get_logger("BridgeUdsReceiveTest");
  hermos::IpcEventLoopBase loop(logger, address, 4, {}, {});
  unsigned calls = 0;
  loop.set_message_handler([&](const void *, size_t) {++calls;});
  ASSERT_TRUE(hermos::send_bridge_uds_message(address, "oversized", 9, logger));
  ASSERT_TRUE(loop.spin_once(100));
  EXPECT_EQ(calls, 0u);
  ASSERT_TRUE(hermos::send_bridge_uds_message(address, "good", 4, logger));
  ASSERT_TRUE(loop.spin_once(100));
  EXPECT_EQ(calls, 1u);
}


TEST(BridgePeerCredentials, RequiresSameUidAndIpcNamespace)
{
  const ucred credentials{getpid(), geteuid(), getegid()};
  const auto ipc_ns = hermos::get_self_ipc_ns_inode();
  EXPECT_TRUE(hermos::detail::bridge_peer_allowed(credentials, geteuid(), ipc_ns));
  const uid_t different_uid = geteuid() == 0 ? 1 : 0;
  EXPECT_FALSE(hermos::detail::bridge_peer_allowed(credentials, different_uid, ipc_ns));
  EXPECT_FALSE(hermos::detail::bridge_peer_allowed(credentials, geteuid(), ipc_ns + 1));
  EXPECT_FALSE(hermos::detail::bridge_peer_allowed(credentials, geteuid(), 0));
  EXPECT_FALSE(hermos::detail::bridge_peer_allowed(ucred{0, geteuid(), getegid()}, geteuid(), ipc_ns));
}

TEST(BridgePeerCredentials, MissingAndMalformedCredentialsAreRejected)
{
  msghdr message{};
  ucred result{};
  EXPECT_FALSE(hermos::detail::read_bridge_credentials(message, result));
  alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(ucred))]{};
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  auto * header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_CREDENTIALS;
  header->cmsg_len = CMSG_LEN(sizeof(ucred)) - 1;
  EXPECT_FALSE(hermos::detail::read_bridge_credentials(message, result));
  header->cmsg_len = sizeof(control) + 1;
  EXPECT_FALSE(hermos::detail::read_bridge_credentials(message, result));
  header->cmsg_len = CMSG_LEN(0) - 1;
  EXPECT_FALSE(hermos::detail::read_bridge_credentials(message, result));
}

TEST(BridgePeerCredentials, CompleteCredentialsAreRequiredWithoutTruncation)
{
  alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(ucred))]{};
  msghdr message{};
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  auto * header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_CREDENTIALS;
  header->cmsg_len = CMSG_LEN(sizeof(ucred));
  const ucred expected{getpid(), geteuid(), getegid()};
  std::memcpy(CMSG_DATA(header), &expected, sizeof(expected));
  ucred result{};
  ASSERT_TRUE(hermos::detail::read_bridge_credentials(message, result));
  EXPECT_EQ(result.pid, expected.pid);
  EXPECT_EQ(result.uid, expected.uid);
  EXPECT_EQ(result.gid, expected.gid);
  for (int flags : {MSG_TRUNC, MSG_CTRUNC}) {
    message.msg_flags = flags;
    EXPECT_FALSE(hermos::detail::read_bridge_credentials(message, result));
  }
}

TEST(BridgePeerCredentials, DuplicateCredentialsAreRejected)
{
  alignas(cmsghdr) unsigned char control[2 * CMSG_SPACE(sizeof(ucred))]{};
  msghdr message{};
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  const ucred expected{getpid(), geteuid(), getegid()};
  auto * header = CMSG_FIRSTHDR(&message);
  for (int index = 0; index < 2; ++index) {
    ASSERT_NE(header, nullptr);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_CREDENTIALS;
    header->cmsg_len = CMSG_LEN(sizeof(ucred));
    std::memcpy(CMSG_DATA(header), &expected, sizeof(expected));
    header = CMSG_NXTHDR(&message, header);
  }
  ucred result{};
  EXPECT_FALSE(hermos::detail::read_bridge_credentials(message, result));
}

TEST(BridgePeerCredentials, UnexpectedRightsAreClosedBeforeRejection)
{
  int pipe_fds[2];
  ASSERT_EQ(pipe(pipe_fds), 0);
  ScopedFd writer(pipe_fds[1]);
  alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int))]{};
  msghdr message{};
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  auto * header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(header), &pipe_fds[0], sizeof(int));
  ucred result{};
  EXPECT_FALSE(hermos::detail::read_bridge_credentials(message, result));
  EXPECT_EQ(fcntl(pipe_fds[0], F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

namespace
{
class TestBridgeEventLoop : public hermos::IpcEventLoopBase
{
public:
  using IpcEventLoopBase::IpcEventLoopBase;
  using IpcEventLoopBase::handle_socket;
};
}  // namespace

TEST(BridgeDebugCredentials, UnknownPeerCannotInvokeStatusHandler)
{
  TestBridgeEventLoop loop(rclcpp::get_logger("BridgeDebugCredentials"),
                          make_unique_abstract_addr("debug_rejected"), 4, {}, {});
  unsigned calls = 0;
  loop.set_socket_handler([&] {++calls; return "private status";});
  loop.handle_socket(-1);
  EXPECT_EQ(calls, 0u);
}

TEST(BridgeDebugCredentials, AuthenticatedLocalPeerReceivesStatus)
{
  TestBridgeEventLoop loop(rclcpp::get_logger("BridgeDebugCredentials"),
                          make_unique_abstract_addr("debug_authorized"), 4, {}, {});
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets), 0);
  ScopedFd server(sockets[0]);
  ScopedFd client(sockets[1]);
  unsigned calls = 0;
  loop.set_socket_handler([&] {++calls; return "status";});
  loop.handle_socket(server.get());
  EXPECT_EQ(calls, 1u);
  char response[16]{};
  ASSERT_EQ(recv(client.get(), response, sizeof(response), 0), 6);
  EXPECT_EQ(std::string(response, 6), "status");
  const uid_t different_uid = geteuid() == 0 ? 1 : 0;
  EXPECT_FALSE(hermos::detail::bridge_stream_peer_allowed(
    server.get(), different_uid, hermos::get_self_ipc_ns_inode()));
}
