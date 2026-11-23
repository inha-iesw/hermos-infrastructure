#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hermos_cie_thread_configurator::detail
{
// Private cgroup-v1 I/O boundary; callers provide a unique owned directory.
bool write_cpuset_value(const std::string & path, const std::string & value);
bool prepare_cpuset(const std::string & path, const std::vector<int> & cpus);
bool attach_cpuset(const std::string & path, int64_t tid);
}  // namespace hermos_cie_thread_configurator::detail
