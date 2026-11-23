#include "hermos_cie_thread_configurator/non_ros_thread_ipc.hpp"
#include "rclcpp/rclcpp.hpp"
#include "ipc/thread_owner.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <limits>
#include <future>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace acie = hermos_cie_thread_configurator;

TEST(NonRosThreadIpc, SocketPathConstantIsNonEmpty)
{
  EXPECT_GT(std::strlen(hermos_cie_thread_configurator::k_non_ros_thread_info_socket_path), 0u);
}

TEST(NonRosThreadIpcEncode, RoundTripEmptyName)
{
  std::array<uint8_t, acie::k_non_ros_thread_info_max_wire_size> buf{};
  size_t buf_len = 0;
  ASSERT_TRUE(acie::encode_non_ros_thread_info({42, ""}, buf.data(), buf.size(), buf_len));
  ASSERT_EQ(buf_len, acie::k_wire_header_size);
  acie::NonRosThreadInfo out;
  ASSERT_TRUE(acie::decode_non_ros_thread_info(buf.data(), buf_len, out));
  EXPECT_EQ(out.tid, 42);
  EXPECT_EQ(out.name, "");
}

TEST(NonRosThreadIpcEncode, RoundTripAsciiName)
{
  std::array<uint8_t, acie::k_non_ros_thread_info_max_wire_size> buf{};
  size_t buf_len = 0;
  ASSERT_TRUE(acie::encode_non_ros_thread_info(
    {123456789, "worker_thread"}, buf.data(), buf.size(), buf_len));
  acie::NonRosThreadInfo out;
  ASSERT_TRUE(acie::decode_non_ros_thread_info(buf.data(), buf_len, out));
  EXPECT_EQ(out.tid, 123456789);
  EXPECT_EQ(out.name, "worker_thread");
}

TEST(NonRosThreadIpcEncode, RoundTripMaxLengthName)
{
  std::string name(65535u, 'x');
  std::array<uint8_t, acie::k_non_ros_thread_info_max_wire_size> buf{};
  size_t buf_len = 0;
  ASSERT_TRUE(acie::encode_non_ros_thread_info({-1, name}, buf.data(), buf.size(), buf_len));
  ASSERT_EQ(buf_len, acie::k_wire_header_size + acie::k_non_ros_thread_info_max_name_len);
  acie::NonRosThreadInfo out;
  ASSERT_TRUE(acie::decode_non_ros_thread_info(buf.data(), buf_len, out));
  EXPECT_EQ(out.tid, -1);
  EXPECT_EQ(out.name, name);
}

TEST(NonRosThreadIpcEncode, RejectsOversizeName)
{
  std::string name(65536u, 'x');
  std::array<uint8_t, acie::k_non_ros_thread_info_max_wire_size> buf{};
  size_t buf_len = 0;
  EXPECT_FALSE(acie::encode_non_ros_thread_info({0, name}, buf.data(), buf.size(), buf_len));
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
TEST(NonRosThreadIpcEncode, GoldenVectorMatchesRust)
{
  // Byte-for-byte cross-language contract. The identical vector is asserted in
  // the Rust crate (rust/src/wire.rs, golden_vector_le); either side drifting
  // fails CI. Native-endian wire format, so little-endian hosts only.
  std::array<uint8_t, acie::k_non_ros_thread_info_max_wire_size> buf{};
  size_t buf_len = 0;
  ASSERT_TRUE(acie::encode_non_ros_thread_info(
    {123456789, "worker_thread"}, buf.data(), buf.size(), buf_len));
  const uint8_t expected[] = {
    0x15, 0xCD, 0x5B, 0x07, 0x00, 0x00, 0x00, 0x00,  // tid = 123456789 (i64 LE)
    0x0D, 0x00,                                      // name_len = 13 (u16 LE)
    'w',  'o',  'r',  'k',  'e',  'r',  '_',  't',  'h', 'r', 'e', 'a', 'd'};
  ASSERT_EQ(buf_len, sizeof(expected));
  EXPECT_EQ(std::memcmp(buf.data(), expected, sizeof(expected)), 0);
}
#endif

TEST(NonRosThreadIpcEncode, RejectsBufferTooSmall)
{
  std::array<uint8_t, 9> tiny{};
  size_t buf_len = 0;
  EXPECT_FALSE(acie::encode_non_ros_thread_info({1, ""}, tiny.data(), tiny.size(), buf_len));
}

TEST(NonRosThreadIpcDecode, RejectsBufferShorterThanHeader)
{
  uint8_t buf[5] = {0};
  acie::NonRosThreadInfo out;
  EXPECT_FALSE(acie::decode_non_ros_thread_info(buf, sizeof(buf), out));
}

TEST(NonRosThreadIpcDecode, RejectsLengthMismatch)
{
  std::array<uint8_t, acie::k_non_ros_thread_info_max_wire_size> buf{};
  size_t buf_len = 0;
  ASSERT_TRUE(acie::encode_non_ros_thread_info({7, "abc"}, buf.data(), buf.size(), buf_len));
  // truncate one byte to break the length invariant
  acie::NonRosThreadInfo out;
  EXPECT_FALSE(acie::decode_non_ros_thread_info(buf.data(), buf_len - 1, out));
}

TEST(NonRosThreadIpcSockaddr, BuildsAbstractAddressWithLeadingNul)
{
  sockaddr_un addr{};
  socklen_t len = acie::setup_non_ros_thread_info_sockaddr(addr);
  EXPECT_EQ(addr.sun_family, AF_UNIX);
  EXPECT_EQ(addr.sun_path[0], '\0');
  EXPECT_EQ(
    std::memcmp(
      addr.sun_path + 1, acie::k_non_ros_thread_info_socket_path,
      std::strlen(acie::k_non_ros_thread_info_socket_path)),
    0);
  EXPECT_EQ(
    static_cast<size_t>(len),
    offsetof(sockaddr_un, sun_path) + 1u + std::strlen(acie::k_non_ros_thread_info_socket_path));
}

namespace
{
// Bind a fresh DGRAM socket on the well-known abstract path. Returns the
// listening fd, or -1 if bind fails (e.g. another listener already running).
// Caller is expected to GTEST_SKIP() when -1 is returned.
int bind_test_listener_or_skip()
{
  int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  socklen_t len = acie::setup_non_ros_thread_info_sockaddr(addr);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), len) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}
}  // namespace

TEST(NonRosThreadIpcSend, DeliversToBoundListener)
{
  int listener = bind_test_listener_or_skip();
  if (listener < 0) {
    GTEST_SKIP() << "bind failed; another configurator is running on this host";
  }

  acie::send_non_ros_thread_info({4242, "test_worker"});

  uint8_t buf[10 + 65535];
  ssize_t n = ::recv(listener, buf, sizeof(buf), 0);
  ASSERT_GT(n, 0);
  acie::NonRosThreadInfo info;
  ASSERT_TRUE(acie::decode_non_ros_thread_info(buf, static_cast<size_t>(n), info));
  EXPECT_EQ(info.tid, 4242);
  EXPECT_EQ(info.name, "test_worker");

  ::close(listener);
}

TEST(NonRosThreadIpcSend, TimesOutWhenNoListener)
{
  // Sanity: ensure no listener exists. If bind succeeds here, we know the
  // path is free; close it immediately so send_non_ros_thread_info sees it
  // as unbound during its retry loop.
  int probe = bind_test_listener_or_skip();
  if (probe < 0) {
    GTEST_SKIP() << "another listener is running; cannot test timeout path";
  }
  ::close(probe);

  const auto t0 = std::chrono::steady_clock::now();
  acie::send_non_ros_thread_info(
    {1, "ghost"}, std::chrono::milliseconds(150), std::chrono::milliseconds(30));
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  EXPECT_GE(elapsed, std::chrono::milliseconds(150));
  EXPECT_LT(elapsed, std::chrono::milliseconds(1000));
}

namespace
{
rclcpp::Logger test_logger()
{
  return rclcpp::get_logger("test_non_ros_thread_ipc");
}
}  // namespace

TEST(NonRosThreadInfoListener, BindsAndDestructsCleanly)
{
  std::atomic<int> calls{0};
  {
    acie::NonRosThreadInfoListener listener(
      [&calls](const acie::NonRosThreadInfo &) { calls.fetch_add(1); }, test_logger());
    // Listener is alive; no messages yet.
  }
  EXPECT_EQ(calls.load(), 0);
}

TEST(NonRosThreadInfoListener, SecondBindThrows)
{
  acie::NonRosThreadInfoListener first([](const acie::NonRosThreadInfo &) {}, test_logger());
  EXPECT_THROW(
    {
      acie::NonRosThreadInfoListener second([](const acie::NonRosThreadInfo &) {}, test_logger());
    },
    std::system_error);
}

TEST(NonRosThreadInfoListener, DispatchesReceivedDatagram)
{
  std::mutex m;
  std::condition_variable cv;
  std::vector<acie::NonRosThreadInfo> received;

  acie::NonRosThreadInfoListener listener(
    [&](const acie::NonRosThreadInfo & info) {
      std::lock_guard<std::mutex> lk(m);
      received.push_back(info);
      cv.notify_one();
    },
    test_logger());

  acie::send_non_ros_thread_info({getpid(), "alpha"});
  acie::send_non_ros_thread_info({getpid(), "beta"});

  std::unique_lock<std::mutex> lk(m);
  ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(2), [&] { return received.size() == 2u; }));

  // SOCK_DGRAM does not formally guarantee order even on the same host, so
  // sort by name before asserting on contents.
  std::sort(
    received.begin(), received.end(),
    [](const acie::NonRosThreadInfo & a, const acie::NonRosThreadInfo & b) {
      return a.name < b.name;
    });
  EXPECT_EQ(received[0].tid, getpid());
  EXPECT_EQ(received[0].name, "alpha");
  EXPECT_EQ(received[1].tid, getpid());
  EXPECT_EQ(received[1].name, "beta");
}

TEST(NonRosThreadInfoListener, DropsMalformedDatagram)
{
  std::atomic<int> calls{0};
  acie::NonRosThreadInfoListener listener(
    [&calls](const acie::NonRosThreadInfo &) { calls.fetch_add(1); }, test_logger());

  // Hand-craft a too-short datagram (< 10 bytes) and send it directly.
  int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_un addr{};
  socklen_t addr_len = acie::setup_non_ros_thread_info_sockaddr(addr);
  uint8_t bad[5] = {0};
  ssize_t s = ::sendto(fd, bad, sizeof(bad), 0, reinterpret_cast<sockaddr *>(&addr), addr_len);
  ASSERT_EQ(static_cast<size_t>(s), sizeof(bad));

  // Now send a well-formed message and wait for it.
  acie::send_non_ros_thread_info({getpid(), "ok"});

  // Give the listener some time. We can't easily synchronize on the dropped
  // message, so wait for the well-formed callback to land.
  for (int i = 0; i < 200 && calls.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(calls.load(), 1);
  ::close(fd);
}

TEST(NonRosThreadInfoListener, StopIsIdempotent)
{
  acie::NonRosThreadInfoListener listener([](const acie::NonRosThreadInfo &) {}, test_logger());
  listener.stop();
  listener.stop();  // must not crash, must not hang
  SUCCEED();
}

TEST(NonRosThreadInfoListener, AfterStopSendTimesOut)
{
  {
    acie::NonRosThreadInfoListener listener([](const acie::NonRosThreadInfo &) {}, test_logger());
    listener.stop();
  }  // listener fully closed by destructor

  const auto t0 = std::chrono::steady_clock::now();
  acie::send_non_ros_thread_info(
    {1, "after_stop"}, std::chrono::milliseconds(100), std::chrono::milliseconds(25));
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  EXPECT_GE(elapsed, std::chrono::milliseconds(100));
}

TEST(NonRosThreadInfoListener, RejectsThreadsOutsideSenderProcess)
{
  std::mutex mutex;
  std::condition_variable ready;
  std::vector<std::string> names;
  acie::NonRosThreadInfoListener listener(
    [&](const acie::NonRosThreadInfo & info) {
      std::lock_guard<std::mutex> lock(mutex);
      names.push_back(info.name);
      ready.notify_one();
    }, test_logger());
  for (int64_t tid : {int64_t{0}, int64_t{-1}, int64_t{getppid()},
                      std::numeric_limits<int64_t>::max()}) {
    acie::send_non_ros_thread_info({tid, "rejected"});
  }
  acie::send_non_ros_thread_info({getpid(), "accepted"});
  std::unique_lock<std::mutex> lock(mutex);
  ASSERT_TRUE(ready.wait_for(lock, std::chrono::seconds(2), [&] {
    return std::find(names.begin(), names.end(), "accepted") != names.end();
  }));
  EXPECT_EQ(names, std::vector<std::string>{"accepted"});
}

TEST(NonRosThreadInfoListener, AcceptsAnotherThreadInSenderProcess)
{
  std::mutex mutex;
  std::condition_variable ready;
  int64_t received_tid = 0;
  acie::NonRosThreadInfoListener listener(
    [&](const acie::NonRosThreadInfo & info) {
      std::lock_guard<std::mutex> lock(mutex);
      received_tid = info.tid;
      ready.notify_one();
    }, test_logger());
  std::promise<int64_t> thread_id;
  std::promise<void> finish;
  auto release = finish.get_future();
  std::thread worker([&] {
    thread_id.set_value(syscall(SYS_gettid));
    release.wait();
  });
  const int64_t tid = thread_id.get_future().get();
  acie::send_non_ros_thread_info({tid, "worker"});
  std::unique_lock<std::mutex> lock(mutex);
  const bool received = ready.wait_for(lock, std::chrono::seconds(2), [&] {
    return received_tid != 0;
  });
  finish.set_value();
  worker.join();
  EXPECT_TRUE(received);
  EXPECT_EQ(received_tid, tid);
}

TEST(NonRosThreadCredentials, AcceptsOnlyTheOwnersThreads)
{
  const ucred own{getpid(), geteuid(), getegid()};
  EXPECT_TRUE(acie::detail::accepts_thread_announcement(getpid(), &own, geteuid()));
  EXPECT_FALSE(acie::detail::accepts_thread_announcement(getpid(), nullptr, geteuid()));
  const ucred foreign{getpid(), static_cast<uid_t>(geteuid() + 1), getegid()};
  EXPECT_FALSE(acie::detail::accepts_thread_announcement(getpid(), &foreign, geteuid()));
  const ucred invalid{0, geteuid(), getegid()};
  EXPECT_FALSE(acie::detail::accepts_thread_announcement(getpid(), &invalid, geteuid()));
  EXPECT_FALSE(acie::detail::accepts_thread_announcement(getppid(), &own, geteuid()));
}
