#include "affinity/cpuset.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace detail = hermos_cie_thread_configurator::detail;

class CpusetFiles : public ::testing::Test
{
protected:
  void SetUp() override
  {
    char path[] = "/tmp/hermos-cpuset-test-XXXXXX";
    const char * created = mkdtemp(path);
    ASSERT_NE(created, nullptr);
    root = created;
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  std::string root;
};

TEST_F(CpusetFiles, MissingControllerIsARejectionWithoutThrowing)
{
  EXPECT_FALSE(detail::prepare_cpuset(root + "/missing/group", {0, 1}));
}

TEST_F(CpusetFiles, NeverReusesOrRemovesAnExistingGroup)
{
  const std::string group = root + "/existing";
  ASSERT_TRUE(std::filesystem::create_directory(group));
  ASSERT_TRUE(detail::write_cpuset_value(group + "/tasks", "original"));
  EXPECT_FALSE(detail::prepare_cpuset(group, {0}));
  std::ifstream value(group + "/tasks");
  std::string contents;
  value >> contents;
  EXPECT_EQ(contents, "original");
}

TEST_F(CpusetFiles, PreparesCpuAndMemoryMasksWithoutMovingThread)
{
  const std::string group = root + "/owned";
  ASSERT_TRUE(detail::prepare_cpuset(group, {0, 2}));
  EXPECT_FALSE(std::filesystem::exists(group + "/tasks"));
  for (const auto & pair : {std::pair{"cpuset.cpus", "0,2"},
                            std::pair{"cpuset.mems", "0"}}) {
    std::ifstream value(group + "/" + pair.first);
    std::string contents;
    value >> contents;
    EXPECT_EQ(contents, pair.second);
  }
}

TEST_F(CpusetFiles, MovesThreadOnlyWhenAttachIsRequested)
{
  const std::string group = root + "/owned";
  ASSERT_TRUE(detail::prepare_cpuset(group, {0}));
  ASSERT_TRUE(detail::attach_cpuset(group, 123));
  std::ifstream value(group + "/tasks");
  std::string contents;
  value >> contents;
  EXPECT_EQ(contents, "123");
}

TEST_F(CpusetFiles, DetectsBufferedWriteFailure)
{
  EXPECT_FALSE(detail::write_cpuset_value("/dev/full", "123"));
  EXPECT_FALSE(detail::write_cpuset_value(root + "/missing/tasks", "123"));
}
