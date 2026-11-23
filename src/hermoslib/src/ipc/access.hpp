#pragma once

#include "hermos/hermos_mq.hpp"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <mqueue.h>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace hermos::detail
{
// The token-capable kmod returns /<128-bit load nonce>-<64-bit topic generation>.
// Earlier modules returned a logical ROS topic (possibly a bridge alias).
inline void require_notification_token(std::string_view token)
{
  constexpr std::string_view hex = "0123456789abcdef";
  if (token.size() != 50 || token.front() != '/' || token[33] != '-' ||
      token.substr(1, 32).find_first_not_of(hex) != std::string_view::npos ||
      token.substr(34).find_first_not_of(hex) != std::string_view::npos) {
    throw std::system_error(
      EPROTONOSUPPORT, std::generic_category(),
      "HERMOS kernel module returned no generation-scoped MQ token; load the matching module");
  }
}

// Validate the opened object rather than a pathname that can be replaced.
inline bool private_ipc_owner(const struct stat & status, uid_t owner)
{
  return S_ISREG(status.st_mode) && status.st_uid == owner &&
         (status.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

inline bool validate_private_heap(int fd, uint64_t expected_size)
{
  struct stat status{};
  if (fstat(fd, &status) != 0) {
    return false;
  }
  if (!private_ipc_owner(status, geteuid())) {
    errno = EACCES;
    return false;
  }
  if (status.st_size < 0 || static_cast<uint64_t>(status.st_size) != expected_size) {
    errno = EINVAL;
    return false;
  }
  return true;
}

// Notification queues have one one-byte slot; published notifications themselves
// are zero-length. Never cache an unrelated or overly accessible existing queue.
inline mqd_t open_private_notification_writer(const char * name)
{
  const mqd_t fd = mq_open(name, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd == -1) {
    return -1;
  }
  struct stat status{};
  struct mq_attr attributes{};
  int error = 0;
  if (fstat(fd, &status) != 0 || mq_getattr(fd, &attributes) != 0) {
    error = errno;
  } else if (!private_ipc_owner(status, geteuid())) {
    error = EACCES;
  } else if (attributes.mq_maxmsg != 1 || attributes.mq_msgsize != sizeof(MqMsgHermos)) {
    error = EINVAL;
  }
  if (error) {
    mq_close(fd);
    errno = error;
    return -1;
  }
  return fd;
}
}  // namespace hermos::detail
