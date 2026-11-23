#include "deadline.hpp"

#include <cerrno>

namespace hermos_cie_thread_configurator::detail
{
DeadlineAffinityResult apply_deadline_affinity(
  const sched_attr & requested, const DeadlineAffinityOperations & operations)
{
  if (!operations.prepare()) {
    return {"prepare cpuset", errno, 0};
  }
  sched_attr previous{};
  previous.size = sizeof(previous);
  if (!operations.get_policy(previous)) {
    const int error = errno;
    operations.cleanup();
    return {"read scheduling policy", error, 0};
  }
  // DL admission needs the original CPU mask. Narrow it only after admission.
  if (!operations.set_policy(requested)) {
    const int error = errno;
    operations.cleanup();
    return {"apply DEADLINE policy", error, 0};
  }
  if (!operations.attach()) {
    const int error = errno;
    const int restore_error = operations.set_policy(previous) ? 0 : errno;
    operations.cleanup();
    return {"attach cpuset", error, restore_error};
  }
  return {};
}
}  // namespace hermos_cie_thread_configurator::detail
