#pragma once

#include "hermos/hermos_epoll.hpp"
#include "hermos/hermos_epoll_update_dispatcher.hpp"
#include "hermos/hermos_public_api.hpp"
#include "rclcpp/rclcpp.hpp"

#include <memory>

namespace hermos
{

extern std::mutex mmap_mtx;

void map_read_only_area(const pid_t pid, const uint64_t shm_addr, const uint64_t shm_size);

struct HermosExecutable
{
  std::shared_ptr<std::function<void()>> callable;
  rclcpp::CallbackGroup::SharedPtr callback_group{nullptr};
};

/** @brief Base class for Stage 1 executors that handle both ROS 2 (RMW) and Hermos callbacks.
 * Inherits from rclcpp::Executor. */
HERMOS_PUBLIC
class HermosExecutor : public rclcpp::Executor
{
  std::mutex ready_hermos_executables_mutex_;
  std::vector<HermosExecutable> ready_hermos_executables_;

  bool get_next_ready_hermos_executable(HermosExecutable & hermos_executable);

  // Return false iff this Executor is SingleThreadedHermosExecutor
  // and used for internal implementation of CallbackIsolatedHermosExecutor
  // and `group` is "not" the callback group dedicated by this Executor.
  virtual bool validate_callback_group(const rclcpp::CallbackGroup::SharedPtr & group) const = 0;

protected:
  std::unique_ptr<EpollManager> epoll_manager_;
  pid_t my_pid_;
  std::mutex wait_mutex_;

  void prepare_epoll();
  bool get_next_hermos_executable(HermosExecutable & hermos_executable, const int timeout_ms);
  static void execute_hermos_executable(HermosExecutable & hermos_executable);

  EpollUpdateTracker epoll_update_tracker_;

public:
  /// Construct the executor.
  /// @param options Executor options.
  HERMOS_PUBLIC
  explicit HermosExecutor(const rclcpp::ExecutorOptions & options = rclcpp::ExecutorOptions());

  virtual ~HermosExecutor();

  /// Block the calling thread and process callbacks in a loop until rclcpp::shutdown() is called or
  /// the executor is cancelled.
  HERMOS_PUBLIC
  virtual void spin() override = 0;
};

}  // namespace hermos
