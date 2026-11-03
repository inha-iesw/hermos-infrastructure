#include "hermos/node/hermos_only_single_threaded_executor.hpp"

#include "hermos/hermos.hpp"

namespace hermos
{

HermosOnlySingleThreadedExecutor::HermosOnlySingleThreadedExecutor(int next_exec_timeout_ms)
: next_exec_timeout_ms_(next_exec_timeout_ms)
{
  const int next_exec_timeout_ms_threshold = 500;  // Rough value
  if (next_exec_timeout_ms_ > next_exec_timeout_ms_threshold) {
    RCLCPP_WARN(
      logger,
      "Due to the large next_exec_timeout_ms value, the callbacks registered after spin and ROS 2 "
      "callbacks may be extremely slow to execute.");
  }

  TRACEPOINT(
    hermos_construct_executor, static_cast<const void *>(this),
    "hermos_only_single_threaded_executor");
}

void HermosOnlySingleThreadedExecutor::spin()
{
  if (spinning_.exchange(true)) {
    RCLCPP_ERROR(logger, "spin() called while already spinning");
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }

  RCPPUTILS_SCOPE_EXIT(this->spinning_.store(false););

  while (spinning_.load() && !cancel_requested_.load() && hermos::ok()) {
    if (epoll_update_tracker_.take_update_request()) {
      add_callback_groups_from_nodes_associated_to_executor();
      epoll_manager_->prepare_epoll([this](const rclcpp::CallbackGroup::SharedPtr & group) {
        return is_callback_group_associated(group);
      });
    }

    hermos::HermosExecutable hermos_executable;
    if (get_next_hermos_executable(hermos_executable, next_exec_timeout_ms_)) {
      execute_hermos_executable(hermos_executable);
    }
  }
}

}  // namespace hermos
