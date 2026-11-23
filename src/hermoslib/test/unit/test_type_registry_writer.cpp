// Copyright 2026
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the process-local `TypeRegistryWriter`. We verify that:
//
//  * the first `register_type()` call creates `<base>/<ns_inode>/<pid>.txt`
//    with the expected `<topic>\t<type>\t<role>\t<node>\n` content,
//  * subsequent calls append (the file grows, not truncates),
//  * concurrent registrations do not interleave within a single line.
//
// We do not exercise the `atexit` cleanup path here — the gtest process
// would have to actually exit to fire it, which is awkward in a single
// binary. The cleanup is best-effort and the daemon's `/proc/<pid>`
// sweep is the load-bearing path.

#include "hermos/hermos_utils.hpp"
#include "hermos/internal/type_registry_writer.hpp"

#include <gtest/gtest.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{

std::string read_file(const std::string & path)
{
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The writer is a process-local singleton. Each test gets a fresh
// `mkdtemp` base dir; `reset_for_test()` then closes the cached fd so
// the next `register_type()` re-opens against the new base.

class TypeRegistryWriterTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    char tmpl[] = "/tmp/hermos_trw_test_XXXXXX";
    const char * created = mkdtemp(tmpl);
    ASSERT_NE(created, nullptr);
    base_dir_ = created;
    hermos::internal::TypeRegistryWriter::set_base_dir_for_test(base_dir_);
    hermos::internal::TypeRegistryWriter::instance().reset_for_test();
  }

  void TearDown() override
  {
    hermos::internal::TypeRegistryWriter::instance().reset_for_test();
    std::error_code ec;
    std::filesystem::remove_all(base_dir_, ec);  // best-effort cleanup
  }

  std::string base_dir_;
};

TEST_F(TypeRegistryWriterTest, RegisterTypeWritesExpectedLine)
{
  auto & writer = hermos::internal::TypeRegistryWriter::instance();
  writer.register_type("/chatter", "std_msgs/msg/Int32", "pub", "/talker_node");

  const std::string path = writer.current_path_for_test();
  ASSERT_FALSE(path.empty());

  // path must be `<base>/<ns_inode>/<pid>.txt`
  const std::string expected_suffix = "/" + std::to_string(getpid()) + ".txt";
  ASSERT_GE(path.size(), expected_suffix.size());
  EXPECT_EQ(path.substr(path.size() - expected_suffix.size()), expected_suffix);
  EXPECT_EQ(path.substr(0, base_dir_.size()), base_dir_);

  const std::string body = read_file(path);
  EXPECT_NE(body.find("/chatter\tstd_msgs/msg/Int32\tpub\t/talker_node\t"), std::string::npos);
  std::istringstream fields(body);
  std::string value;
  for (int index = 0; index < 4; ++index) ASSERT_TRUE(std::getline(fields, value, '\t'));
  ASSERT_TRUE(std::getline(fields, value, '\t'));
  EXPECT_EQ(std::stoul(value), hermos::get_ros_domain_id());
  ASSERT_TRUE(std::getline(fields, value));
  EXPECT_GT(std::stoull(value), 0U);
  const std::string process = read_file("/proc/self/stat");
  std::istringstream identity(process.substr(process.rfind(')') + 2));
  std::string start;
  for (int field = 3; field <= 22; ++field) ASSERT_TRUE(identity >> start);
  EXPECT_EQ(value, start);
}

TEST_F(TypeRegistryWriterTest, RegisterTypeAppendsAdditionalLines)
{
  auto & writer = hermos::internal::TypeRegistryWriter::instance();
  writer.register_type("/topic_a", "std_msgs/msg/Int32", "pub", "/node_a");
  writer.register_type("/topic_b", "std_msgs/msg/String", "sub", "/node_b");

  const std::string body = read_file(writer.current_path_for_test());
  // Both lines should be present (order matches call order).
  const auto pos_a = body.find("/topic_a\tstd_msgs/msg/Int32\tpub\t/node_a\t");
  const auto pos_b = body.find("/topic_b\tstd_msgs/msg/String\tsub\t/node_b\t");
  ASSERT_NE(pos_a, std::string::npos);
  ASSERT_NE(pos_b, std::string::npos);
  EXPECT_LT(pos_a, pos_b);
}

TEST_F(TypeRegistryWriterTest, ConcurrentRegisterTypeDoesNotInterleaveLines)
{
  auto & writer = hermos::internal::TypeRegistryWriter::instance();
  constexpr int kThreads = 8;
  constexpr int kPerThread = 32;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t, &writer]() {
      for (int i = 0; i < kPerThread; ++i) {
        writer.register_type(
          "/topic_" + std::to_string(t), "std_msgs/msg/Int32", "pub",
          "/node_" + std::to_string(t) + "_" + std::to_string(i));
      }
    });
  }
  for (auto & th : threads) {
    th.join();
  }

  // Every line carries domain and process-start identity without interleaving.
  const std::string body = read_file(writer.current_path_for_test());
  std::stringstream ss(body);
  std::string line;
  int count = 0;
  while (std::getline(ss, line)) {
    int tabs = 0;
    for (char c : line) {
      if (c == '\t') ++tabs;
    }
    EXPECT_EQ(tabs, 5) << "garbled line: " << line;
    ++count;
  }
  // Every register_type call must produce exactly one line.
  EXPECT_EQ(count, kThreads * kPerThread);
}

TEST_F(TypeRegistryWriterTest, CreatesPrivateNamespaceDirectoryAndFile)
{
  auto & writer = hermos::internal::TypeRegistryWriter::instance();
  writer.register_type("/topic", "Type", "pub", "/node");
  const std::string path = writer.current_path_for_test();
  ASSERT_FALSE(path.empty());
  for (const auto & object : {std::filesystem::path(path), std::filesystem::path(path).parent_path()}) {
    struct stat metadata{};
    ASSERT_EQ(stat(object.c_str(), &metadata), 0);
    EXPECT_EQ(metadata.st_uid, geteuid());
    EXPECT_EQ(metadata.st_mode & 0077, 0);
  }
}

TEST_F(TypeRegistryWriterTest, RejectsPublicBaseWithoutChangingItsPermissions)
{
  ASSERT_EQ(chmod(base_dir_.c_str(), 0755), 0);
  auto & writer = hermos::internal::TypeRegistryWriter::instance();
  writer.register_type("/topic", "Type", "pub", "/node");
  EXPECT_TRUE(writer.current_path_for_test().empty());
  struct stat metadata{};
  ASSERT_EQ(stat(base_dir_.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0755);
  EXPECT_TRUE(std::filesystem::is_empty(base_dir_));
}

TEST_F(TypeRegistryWriterTest, RejectsSymlinkNamespaceWithoutWritingItsTarget)
{
  const auto target = std::filesystem::path(base_dir_) / "target";
  ASSERT_EQ(mkdir(target.c_str(), 0700), 0);
  std::filesystem::create_directory_symlink(
    target, std::filesystem::path(base_dir_) / std::to_string(hermos::get_self_ipc_ns_inode()));
  auto & writer = hermos::internal::TypeRegistryWriter::instance();
  writer.register_type("/topic", "Type", "pub", "/node");
  EXPECT_TRUE(writer.current_path_for_test().empty());
  EXPECT_TRUE(std::filesystem::is_empty(target));
}

TEST_F(TypeRegistryWriterTest, RejectsUnsafeExistingFilesWithoutAppending)
{
  const auto ns_dir = std::filesystem::path(base_dir_) / std::to_string(hermos::get_self_ipc_ns_inode());
  ASSERT_EQ(mkdir(ns_dir.c_str(), 0700), 0);
  const auto path = ns_dir / (std::to_string(getpid()) + ".txt");
  const auto target = std::filesystem::path(base_dir_) / "target.txt";
  for (const std::string kind : {"public", "symlink", "hardlink"}) {
    SCOPED_TRACE(kind);
    hermos::internal::TypeRegistryWriter::instance().reset_for_test();
    std::ofstream(target) << "original";
    ASSERT_EQ(chmod(target.c_str(), 0600), 0);
    if (kind == "public") {
      std::filesystem::rename(target, path);
      ASSERT_EQ(chmod(path.c_str(), 0644), 0);
    } else if (kind == "symlink") {
      std::filesystem::create_symlink(target, path);
    } else {
      std::filesystem::create_hard_link(target, path);
    }
    auto & writer = hermos::internal::TypeRegistryWriter::instance();
    writer.register_type("/topic", "Type", "pub", "/node");
    EXPECT_TRUE(writer.current_path_for_test().empty());
    EXPECT_EQ(read_file(path.string()), "original");
    std::filesystem::remove(path);
    std::filesystem::remove(target);
  }
}

TEST_F(TypeRegistryWriterTest, RejectsSymlinkBaseWithoutWritingItsTarget)
{
  const auto target = std::filesystem::path(base_dir_) / "target";
  const auto alias = std::filesystem::path(base_dir_) / "alias";
  ASSERT_EQ(mkdir(target.c_str(), 0700), 0);
  std::filesystem::create_directory_symlink(target, alias);
  hermos::internal::TypeRegistryWriter::set_base_dir_for_test(alias.string());
  auto & writer = hermos::internal::TypeRegistryWriter::instance();
  writer.register_type("/topic", "Type", "pub", "/node");
  EXPECT_TRUE(writer.current_path_for_test().empty());
  EXPECT_TRUE(std::filesystem::is_empty(target));
}

TEST_F(TypeRegistryWriterTest, RejectsPublicNamespaceWithoutChangingIt)
{
  const auto ns_dir = std::filesystem::path(base_dir_) / std::to_string(hermos::get_self_ipc_ns_inode());
  ASSERT_EQ(mkdir(ns_dir.c_str(), 0700), 0);
  ASSERT_EQ(chmod(ns_dir.c_str(), 0755), 0);
  auto & writer = hermos::internal::TypeRegistryWriter::instance();
  writer.register_type("/topic", "Type", "pub", "/node");
  EXPECT_TRUE(writer.current_path_for_test().empty());
  struct stat metadata{};
  ASSERT_EQ(stat(ns_dir.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0755);
  EXPECT_TRUE(std::filesystem::is_empty(ns_dir));
}

TEST_F(TypeRegistryWriterTest, ReopensPrivateFileWithoutTruncatingIt)
{
  auto & writer = hermos::internal::TypeRegistryWriter::instance();
  writer.register_type("/first", "Type", "pub", "/node");
  const std::string path = writer.current_path_for_test();
  const std::string first = read_file(path);
  writer.reset_for_test();
  writer.register_type("/second", "Type", "sub", "/node");
  EXPECT_EQ(writer.current_path_for_test(), path);
  const auto body = read_file(path);
  EXPECT_EQ(body.substr(0, first.size()), first);
  EXPECT_NE(body.find("/second\tType\tsub\t/node\t"), std::string::npos);
}

TEST_F(TypeRegistryWriterTest, RejectsExistingFifoWithoutBlocking)
{
  const auto ns_dir = std::filesystem::path(base_dir_) / std::to_string(hermos::get_self_ipc_ns_inode());
  ASSERT_EQ(mkdir(ns_dir.c_str(), 0700), 0);
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const auto path = ns_dir / (std::to_string(getpid()) + ".txt");
    if (mkfifo(path.c_str(), 0600) != 0) _exit(2);
    // A blocking open would otherwise hang the entire host gate.
    alarm(2);
    auto & writer = hermos::internal::TypeRegistryWriter::instance();
    writer.register_type("/topic", "Type", "pub", "/node");
    _exit(writer.current_path_for_test().empty() ? 0 : 1);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

}  // namespace
