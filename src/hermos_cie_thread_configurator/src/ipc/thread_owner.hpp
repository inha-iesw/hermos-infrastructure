#pragma once

#include <sys/socket.h>
#include <sys/types.h>

#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace hermos_cie_thread_configurator::detail
{
// Receive-time credential policy. This does not turn numeric TIDs into stable
// task handles or authenticate announcements received through DDS.
inline bool accepts_thread_announcement(int64_t tid, const ucred * sender, uid_t owner)
{
  if (!sender || sender->uid != owner || tid <= 0 || tid > std::numeric_limits<pid_t>::max() || sender->pid <= 0) {
    return false;
  }
  std::ifstream status("/proc/" + std::to_string(tid) + "/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.compare(0, 5, "Tgid:") == 0) {
      std::istringstream value(line.substr(5));
      pid_t tgid = 0;
      return (value >> tgid) && tgid == sender->pid;
    }
  }
  return false;
}
}  // namespace hermos_cie_thread_configurator::detail
