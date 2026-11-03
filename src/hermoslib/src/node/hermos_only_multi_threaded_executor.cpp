#include "hermos/node/hermos_only_multi_threaded_executor.hpp"

#include "hermos/hermos.hpp"

#include <algorithm>

namespace hermos
{

HermosOnlyMultiThreadedExecutor::HermosOnlyMultiThreadedExecutor(
  size_t number_of_threads, bool yield_before_execute, int next_exec_timeout_ms)
: number_of_threads_(
    number_of_threads != 0 ? number_of_threads
                           : std::max<size_t>(1, std::thread::hardware_concurrency())),
  yield_before_execute_(yield_before_execute),
  next_exec_timeout_ms_(next_exec_timeout_ms)
{
  TRACEPOINT(
    hermos_construct_executor, static_cast<const void *>(this),
    "hermos_only_multi_threaded_executor");
}

void HermosOnlyMultiThreadedExecutor::spin()
{
  if (spinning_.exchange(true)) {
    RCLCPP_ERROR(logger, "spin() called while already spinning");
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }

  RCPPUTILS_SCOPE_EXIT(this->spinning_.store(false););

  if (cancel_requested_.load()) {
    return;
  }

  std::vector<std::thread> threads;

  for (size_t i = 0; i < number_of_threads_ - 1; i++) {
    auto func = [this] { hermos_spin(); };
    threads.emplace_back(func);
  }

  hermos_spin();

  for (auto & thread : threads) {
    thread.join();
  }
}

void HermosOnlyMultiThreadedExecutor::hermos_spin()
{
  while (spinning_.load() && !cancel_requested_.load() && hermos::ok()) {
    if (epoll_update_tracker_.take_update_request()) {
      add_callback_groups_from_nodes_associated_to_executor();
      epoll_manager_->prepare_epoll([this](const rclcpp::CallbackGroup::SharedPtr & group) {
        return is_callback_group_associated(group);
      });
    }

    hermos::HermosExecutable hermos_executable;

    if (!spinning_.load() || cancel_requested_.load() || !hermos::ok()) {
      return;
    }

    // As each thread is dedicated to handling Hermos callbacks, get_next_hermos_executable()
    // can block indefinitely without a timeout. However, since we need to periodically check for
    // epoll updates, we should implement a long timeout period instead of an infinite block.
    if (get_next_hermos_executable(
          hermos_executable, next_exec_timeout_ms_ /* timed-blocking*/)) {
      if (yield_before_execute_) {
        std::this_thread::yield();
      }

      execute_hermos_executable(hermos_executable);
    }
  }
}

}  // namespace hermos
