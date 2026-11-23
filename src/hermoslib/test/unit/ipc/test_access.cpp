#include "hermos/hermos.hpp"
#include "hermos/hermos_subscription.hpp"
#include "hermos/hermos_utils.hpp"
#include "ipc/access.hpp"

#include <gtest/gtest.h>
#include <fcntl.h>
#include <mqueue.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hermos
{
void * map_area(pid_t pid, uint64_t address, uint64_t size, bool writable);
extern std::vector<int> shm_fds;
}

class NativeIpcAccess : public ::testing::Test
{
protected:
  void SetUp() override
  {
    // These tests never initialize Hermos or load a kmod. Negative IDs keep their
    // owned SHM names distinct from real publisher processes in the same namespace.
    fake_pid = -getpid();
    shm_name = hermos::create_shm_name(fake_pid);
    topic = "/hermos_ipc_test_" + std::to_string(getpid());
    mq_name = hermos::create_mq_name_for_hermos_publish(topic, 0);
    address = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(address, MAP_FAILED);
    ASSERT_EQ(munmap(address, size), 0);
  }

  void TearDown() override
  {
    if (mapped) {munmap(address, size);}
    for (int fd : hermos::shm_fds) {close(fd);}
    hermos::shm_fds.clear();
    if (owns_shm) {shm_unlink(shm_name.c_str());}
    if (queue.first != -1) {hermos::remove_mq(queue);}
  }

  pid_t fake_pid;
  std::string shm_name;
  std::string mq_name;
  std::string topic;
  std::pair<mqd_t, std::string> queue{-1, ""};
  const size_t size = 4096;
  void * address = nullptr;
  bool mapped = false;
  bool owns_shm = false;
};

TEST_F(NativeIpcAccess, SharedHeapIsReadableOnlyByItsOwner)
{
  void * result = hermos::map_area(fake_pid, reinterpret_cast<uint64_t>(address), size, true);
  ASSERT_EQ(result, address);
  mapped = owns_shm = true;
  const int fd = shm_open(shm_name.c_str(), O_RDONLY, 0);
  ASSERT_GE(fd, 0);
  struct stat status{};
  ASSERT_EQ(fstat(fd, &status), 0);
  EXPECT_EQ(status.st_uid, geteuid());
  EXPECT_EQ(status.st_mode & 0777, 0400);
  close(fd);
}

TEST_F(NativeIpcAccess, NotificationQueueIsPrivateToItsOwner)
{
  const auto fd = hermos::open_mq_for_subscription(topic, 0, queue);
  ASSERT_GE(fd, 0);
  struct stat status{};
  ASSERT_EQ(fstat(fd, &status), 0);
  EXPECT_EQ(status.st_uid, geteuid());
  EXPECT_EQ(status.st_mode & 0077, 0);
}

TEST_F(NativeIpcAccess, ExistingNotificationQueueIsNeverAdoptedOrRemoved)
{
  const auto fd = hermos::open_mq_for_subscription(topic, 0, queue);
  ASSERT_GE(fd, 0);
  const auto writer = mq_open(mq_name.c_str(), O_WRONLY);
  ASSERT_GE(writer, 0);
  ASSERT_EQ(mq_send(writer, "", 0, 0), 0);
  mq_close(writer);
  EXPECT_EXIT({
    auto replacement = std::make_pair(static_cast<mqd_t>(-1), std::string{});
    hermos::open_mq_for_subscription(topic, 0, replacement);
    _exit(0);
  }, ::testing::ExitedWithCode(EXIT_FAILURE), "mq_open failed");
  struct mq_attr attr{};
  ASSERT_EQ(mq_getattr(fd, &attr), 0);
  EXPECT_EQ(attr.mq_curmsgs, 1);
}

TEST(NativeIpcOwnership, DifferentUidAndPublicModesAreRejected)
{
  struct stat status{};
  status.st_uid = 123;
  status.st_mode = S_IFREG | 0600;
  EXPECT_TRUE(hermos::detail::private_ipc_owner(status, 123));
  EXPECT_FALSE(hermos::detail::private_ipc_owner(status, 124));
  for (mode_t mode : {S_IFREG | 0640, S_IFREG | 0604, S_IFDIR | 0700}) {
    status.st_mode = mode;
    EXPECT_FALSE(hermos::detail::private_ipc_owner(status, 123));
  }
}

TEST_F(NativeIpcAccess, ReadMappingRejectsPublicPermissionsBeforeMmap)
{
  const int fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  ASSERT_GE(fd, 0);
  owns_shm = true;
  ASSERT_EQ(ftruncate(fd, size), 0);
  ASSERT_EQ(fchmod(fd, 0444), 0);
  EXPECT_EQ(hermos::map_area(fake_pid, reinterpret_cast<uint64_t>(address), size, false), nullptr);
  EXPECT_TRUE(hermos::shm_fds.empty());
  const int remaining = shm_open(shm_name.c_str(), O_RDONLY, 0);
  EXPECT_GE(remaining, 0);
  if (remaining >= 0) {close(remaining);}
  close(fd);
}

TEST_F(NativeIpcAccess, ReadMappingRejectsWrongSizeBeforeMmap)
{
  const int fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  ASSERT_GE(fd, 0);
  owns_shm = true;
  ASSERT_EQ(ftruncate(fd, size / 2), 0);
  ASSERT_EQ(fchmod(fd, 0400), 0);
  EXPECT_EQ(hermos::map_area(fake_pid, reinterpret_cast<uint64_t>(address), size, false), nullptr);
  EXPECT_TRUE(hermos::shm_fds.empty());
  close(fd);
}

TEST_F(NativeIpcAccess, SameOwnerPrivateHeapCanBeRead)
{
  const int fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  ASSERT_GE(fd, 0);
  owns_shm = true;
  ASSERT_EQ(ftruncate(fd, size), 0);
  const char payload[] = "private payload";
  ASSERT_EQ(pwrite(fd, payload, sizeof(payload), 0), static_cast<ssize_t>(sizeof(payload)));
  ASSERT_EQ(fchmod(fd, 0400), 0);
  ASSERT_EQ(hermos::map_area(fake_pid, reinterpret_cast<uint64_t>(address), size, false), address);
  mapped = true;
  EXPECT_STREQ(static_cast<const char *>(address), payload);
  close(fd);
}

TEST_F(NativeIpcAccess, SameOwnerNotificationWriterCanSend)
{
  const auto reader = hermos::open_mq_for_subscription(topic, 0, queue);
  const auto writer = hermos::detail::open_private_notification_writer(mq_name.c_str());
  ASSERT_GE(writer, 0);
  ASSERT_EQ(mq_send(writer, "", 0, 0), 0);
  char message{};
  EXPECT_EQ(mq_receive(reader, &message, 1, nullptr), 0);
  mq_close(writer);
}

TEST_F(NativeIpcAccess, NotificationWriterRejectsPublicQueueWithoutRemovingIt)
{
  const auto reader = hermos::open_mq_for_subscription(topic, 0, queue);
  ASSERT_EQ(fchmod(reader, 0666), 0);
  EXPECT_EQ(hermos::detail::open_private_notification_writer(mq_name.c_str()), -1);
  EXPECT_EQ(errno, EACCES);
  struct mq_attr attr{};
  EXPECT_EQ(mq_getattr(reader, &attr), 0);
}

TEST_F(NativeIpcAccess, NotificationWriterRejectsUnrelatedQueueShape)
{
  struct mq_attr attr{};
  attr.mq_maxmsg = 2;
  attr.mq_msgsize = 8;
  const auto reader = mq_open(mq_name.c_str(), O_CREAT | O_EXCL | O_RDONLY | O_NONBLOCK, 0600, &attr);
  ASSERT_GE(reader, 0);
  queue = {reader, mq_name};
  EXPECT_EQ(hermos::detail::open_private_notification_writer(mq_name.c_str()), -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(mq_getattr(reader, &attr), 0);
}
