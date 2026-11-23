#pragma once

#include <cstdint>
#include <functional>
#include <unistd.h>

#include "hermos_cie_thread_configurator/sched_deadline.hpp"

namespace hermos_cie_thread_configurator::detail
{
// Private syscall boundary: prepare owns its new group only on success.
struct DeadlineAffinityOperations
{
  std::function<bool()> prepare;
  std::function<bool(sched_attr &)> get_policy;
  std::function<bool(const sched_attr &)> set_policy;
  std::function<bool()> attach;
  std::function<void()> cleanup;
};

struct DeadlineAffinityResult
{
  const char * failed_operation = nullptr;
  int error = 0;
  int restore_error = 0;
};

DeadlineAffinityResult apply_deadline_affinity(
  const sched_attr & requested, const DeadlineAffinityOperations & operations);
}  // namespace hermos_cie_thread_configurator::detail
