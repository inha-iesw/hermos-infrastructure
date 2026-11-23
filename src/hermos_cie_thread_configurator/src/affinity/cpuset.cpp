#include "cpuset.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace hermos_cie_thread_configurator::detail
{
bool write_cpuset_value(const std::string & path, const std::string & value)
{
  std::ofstream output(path);
  if (!output) {
    return false;
  }
  output << value;
  output.close();
  return !output.fail();
}

bool prepare_cpuset(const std::string & path, const std::vector<int> & cpus)
{
  std::error_code error;
  if (!std::filesystem::create_directory(path, error)) {
    errno = error ? error.value() : EEXIST;
    return false;
  }
  std::ostringstream cpu_list;
  for (size_t i = 0; i < cpus.size(); ++i) {
    if (i) {
      cpu_list << ',';
    }
    cpu_list << cpus[i];
  }
  if (!write_cpuset_value(path + "/cpuset.cpus", cpu_list.str()) ||
      !write_cpuset_value(path + "/cpuset.mems", "0")) {
    // cgroup pseudo-files do not prevent removing the directory. Never remove
    // a path that create_directory did not create for this invocation.
    const int write_error = errno;
    std::filesystem::remove(path, error);
    errno = write_error;
    return false;
  }
  return true;
}
bool attach_cpuset(const std::string & path, int64_t tid)
{
  return write_cpuset_value(path + "/tasks", std::to_string(tid));
}
}  // namespace hermos_cie_thread_configurator::detail
