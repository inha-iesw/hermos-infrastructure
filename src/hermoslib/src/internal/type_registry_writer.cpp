// Copyright 2026
// SPDX-License-Identifier: Apache-2.0

#include "hermos/internal/type_registry_writer.hpp"

#include "hermos/hermos_utils.hpp"

#include <rclcpp/logging.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace hermos::internal
{

namespace
{
// The discovery reader accepts only same-euid, private registry objects.
constexpr mode_t kRegistryDirMode = 0700;
constexpr mode_t kRegistryFileMode = 0600;

// Tmpfs root. Default `/dev/shm` is world-writable (1777) so unelevated
// user processes can create entries; override via `HERMOS_TMPFS_DIR`
// for hardened containers. Test seam: `set_base_dir_for_test()`.
std::string g_base_dir = []() {
  const char * env = std::getenv("HERMOS_TMPFS_DIR");
  std::string root = (env != nullptr && *env != '\0') ? env : "/dev/shm";
  return root + "/hermos_type_registry_" + std::to_string(geteuid());
}();  // NOLINT(runtime/string)

int require_private_owner(int fd, bool directory)
{
  if (fd == -1) return -1;
  struct stat metadata{};
  if (fstat(fd, &metadata) == 0) {
    const bool correct_type = directory ? S_ISDIR(metadata.st_mode) : S_ISREG(metadata.st_mode);
    if (correct_type && metadata.st_uid == geteuid() && !(metadata.st_mode & 0077) &&
        (directory || metadata.st_nlink == 1)) {
      return fd;
    }
    errno = EACCES;
  }
  const int error = errno;
  close(fd);
  errno = error;
  return -1;
}

int open_private_directory(int parent_fd, const std::string & name)
{
  if (mkdirat(parent_fd, name.c_str(), kRegistryDirMode) != 0 && errno != EEXIST) {
    return -1;
  }
  return require_private_owner(
    openat(parent_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC), true);
}

int open_private_file(int parent_fd, const std::string & name)
{
  // NONBLOCK prevents a pre-existing FIFO from blocking before fstat rejects it.
  const int flags = O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
  int fd = openat(parent_fd, name.c_str(), flags | O_CREAT | O_EXCL, kRegistryFileMode);
  if (fd == -1 && errno == EEXIST) {
    fd = openat(parent_fd, name.c_str(), flags);
  }
  return require_private_owner(fd, false);
}

std::string process_start_ticks()
{
  std::ifstream input("/proc/self/stat");
  std::string line;
  if (!std::getline(input, line)) return "0";
  const auto command_end = line.rfind(')');
  if (command_end == std::string::npos) return "0";
  std::istringstream fields(line.substr(command_end + 1));
  std::string field;
  for (int number = 3; number <= 22; ++number) {
    if (!(fields >> field)) return "0";
  }
  return field;
}
}  // namespace

TypeRegistryWriter & TypeRegistryWriter::instance()
{
  static TypeRegistryWriter inst;
  return inst;
}

void TypeRegistryWriter::set_base_dir_for_test(const std::string & dir)
{
  g_base_dir = dir;
}

std::string TypeRegistryWriter::current_path_for_test() const
{
  return path_;
}

void TypeRegistryWriter::ensure_open_locked()
{
  if (fd_ != -1) {
    return;
  }
  if (open_failed_warned_) {
    return;
  }

  uint64_t ns_inode = 0;
  try {
    ns_inode = get_self_ipc_ns_inode();
  } catch (const std::exception & e) {
    RCLCPP_WARN(
      rclcpp::get_logger("Hermos"), "TypeRegistryWriter: failed to read IPC namespace inode: %s",
      e.what());
    open_failed_warned_ = true;
    return;
  }

  // Any failure here means cross-NS observability silently breaks for
  // this process, so log as ERROR (not a transient warning).
  const int base_fd = open_private_directory(AT_FDCWD, g_base_dir);
  if (base_fd == -1) {
    RCLCPP_ERROR(
      rclcpp::get_logger("Hermos"),
      "TypeRegistryWriter: open private directory '%s' failed: %s. Cross-NS observability will "
      "silently NOT work in this process. Override the root with "
      "HERMOS_TMPFS_DIR if /dev/shm is unavailable.",
      g_base_dir.c_str(), std::strerror(errno));
    open_failed_warned_ = true;
    return;
  }
  const std::string ns_name = std::to_string(ns_inode);
  const std::string ns_dir = g_base_dir + "/" + ns_name;
  const int ns_fd = open_private_directory(base_fd, ns_name);
  const int ns_error = errno;
  close(base_fd);
  if (ns_fd == -1) {
    RCLCPP_ERROR(
      rclcpp::get_logger("Hermos"),
      "TypeRegistryWriter: open private directory '%s' failed: %s.", ns_dir.c_str(),
      std::strerror(ns_error));
    open_failed_warned_ = true;
    return;
  }

  const std::string filename = std::to_string(getpid()) + ".txt";
  fd_ = open_private_file(ns_fd, filename);
  const int file_error = errno;
  close(ns_fd);
  if (fd_ == -1) {
    RCLCPP_ERROR(
      rclcpp::get_logger("Hermos"),
      "TypeRegistryWriter: open private file '%s/%s' failed: %s. Check the parent directory "
      "permissions and the tmpfs free space.",
      ns_dir.c_str(), filename.c_str(), std::strerror(file_error));
    open_failed_warned_ = true;
    return;
  }
  // A rejected existing file must never become an atexit cleanup target.
  path_ = ns_dir + "/" + filename;
  // Kernel process registration fixes its domain for its lifetime. Keep the
  // writer's identity stable too; a zero start identity permits type hints only,
  // never verified PID attribution.
  identity_suffix_ = "\t" + std::to_string(get_ros_domain_id()) + "\t" + process_start_ticks();

  // Register a one-shot atexit handler the first time we open the file
  // successfully. SIGKILL bypasses this; the daemon's `/proc/<pid>` sweep
  // handles those stale files (and also any case where registration fails).
  static bool atexit_registered = false;
  if (!atexit_registered && std::atexit(&TypeRegistryWriter::on_process_exit) == 0) {
    atexit_registered = true;
  }
}

void TypeRegistryWriter::register_type(
  const std::string & topic_name, const std::string & type_name, const std::string & role,
  const std::string & node_name)
{
  std::lock_guard<std::mutex> lock(mutex_);
  ensure_open_locked();
  if (fd_ == -1) {
    return;
  }

  std::string line;
  line.reserve(topic_name.size() + type_name.size() + role.size() + node_name.size() +
               identity_suffix_.size() + 4);
  line.append(topic_name).push_back('\t');
  line.append(type_name).push_back('\t');
  line.append(role).push_back('\t');
  line.append(node_name).append(identity_suffix_).push_back('\n');

  // One file per process plus the mutex above keeps writers from interleaving.
  // We loop to drain short writes and retry on EINTR. A partial line is only
  // possible if a later `write()` fails outright (e.g. ENOSPC), in which case
  // the daemon's parser skips the unterminated tail.
  const char * data = line.data();
  size_t remaining = line.size();
  while (remaining > 0) {
    const ssize_t n = ::write(fd_, data, remaining);
    if (n > 0) {
      data += n;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      remaining -= static_cast<size_t>(n);
      continue;
    }
    if (n == -1 && errno == EINTR) {
      continue;
    }
    RCLCPP_WARN_ONCE(
      rclcpp::get_logger("Hermos"),
      "TypeRegistryWriter: write to '%s' failed: %s. Further write failures will be silent.",
      path_.c_str(), std::strerror(errno));
    return;
  }
}

void TypeRegistryWriter::reset_for_test()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ != -1) {
    ::close(fd_);
    fd_ = -1;
  }
  path_.clear();
  identity_suffix_.clear();
  open_failed_warned_ = false;
}

void TypeRegistryWriter::on_process_exit()
{
  auto & inst = TypeRegistryWriter::instance();
  std::lock_guard<std::mutex> lock(inst.mutex_);
  if (inst.fd_ != -1) {
    ::close(inst.fd_);
    inst.fd_ = -1;
  }
  if (!inst.path_.empty()) {
    // Best-effort. If unlink fails the daemon's `cleanup_dead_pids()` will
    // notice the dead pid and remove the file.
    (void)::unlink(inst.path_.c_str());
  }
}

}  // namespace hermos::internal
