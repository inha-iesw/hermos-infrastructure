#pragma once

#include <rclcpp/logger.hpp>
#include <rclcpp/logging.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

namespace hermos
{

inline constexpr int BRIDGE_UDS_SEND_MAX_RETRIES = 100;
inline constexpr useconds_t BRIDGE_UDS_SEND_RETRY_INTERVAL_US = 100000;

namespace detail
{

// Abstract-namespace addresses are length-scoped: leading NUL + name bytes,
// no trailing NUL. Returns the socklen_t bind()/sendto() expect.
inline socklen_t fill_abstract_sockaddr(const std::string & addr, sockaddr_un & out)
{
  out = {};
  out.sun_family = AF_UNIX;
  if (addr.empty() || addr[0] != '\0') {
    throw std::invalid_argument("abstract UDS address must start with NUL");
  }
  if (addr.size() > sizeof(out.sun_path)) {
    throw std::length_error("abstract UDS address too long for sun_path");
  }
  std::memcpy(out.sun_path, addr.data(), addr.size());
  return static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + addr.size());
}

// Credentials must come from recvmsg()/SO_PEERCRED, never from the wire payload.
// A vanished or inaccessible sender is not admitted based on a socket name.
inline bool bridge_peer_allowed(
  const ucred & credentials, uid_t owner_uid, uint64_t ipc_ns_inode)
{
  if (credentials.pid <= 0 || credentials.uid != owner_uid || ipc_ns_inode == 0) {
    return false;
  }
  struct stat namespace_stat{};
  const auto path = "/proc/" + std::to_string(credentials.pid) + "/ns/ipc";
  return stat(path.c_str(), &namespace_stat) == 0 &&
         static_cast<uint64_t>(namespace_stat.st_ino) == ipc_ns_inode;
}

// Only a single complete SCM_CREDENTIALS record is accepted. Unexpected rights
// may have installed descriptors before admission; close them even on rejection.
inline bool read_bridge_credentials(msghdr & message, ucred & credentials)
{
  bool valid = (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) == 0;
  bool found = false;
  for (auto * header = CMSG_FIRSTHDR(&message); header;
       header = CMSG_NXTHDR(&message, header)) {
    const auto remaining = message.msg_controllen -
      (reinterpret_cast<unsigned char *>(header) -
       static_cast<unsigned char *>(message.msg_control));
    if (header->cmsg_len < CMSG_LEN(0) || header->cmsg_len > remaining) {
      return false;
    }
    if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
      const size_t bytes = header->cmsg_len - CMSG_LEN(0);
      for (size_t offset = 0; offset + sizeof(int) <= bytes; offset += sizeof(int)) {
        int fd;
        std::memcpy(&fd, CMSG_DATA(header) + offset, sizeof(fd));
        close(fd);
      }
      valid = false;
    } else if (
      header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_CREDENTIALS &&
      header->cmsg_len == CMSG_LEN(sizeof(ucred)) && !found) {
      std::memcpy(&credentials, CMSG_DATA(header), sizeof(credentials));
      found = true;
    } else {
      valid = false;
    }
  }
  return valid && found;
}

inline bool bridge_stream_peer_allowed(int fd, uid_t owner_uid, uint64_t ipc_ns_inode)
{
  ucred credentials{};
  socklen_t size = sizeof(credentials);
  return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
         size == sizeof(credentials) && bridge_peer_allowed(credentials, owner_uid, ipc_ns_inode);
}

}  // namespace detail

/**
 * @brief Bind a nonblocking bridge socket with kernel sender credentials enabled.
 * @param addr Length-scoped abstract Unix socket address, including its leading NUL.
 * @return Owned listener fd; the caller must close it and authenticate each recvmsg result.
 * @throws std::system_error If socket creation, credential setup or binding fails.
 */
inline int create_bridge_uds_listener(const std::string & addr)
{
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    throw std::system_error(errno, std::generic_category(), "bridge UDS socket() failed");
  }

  try {
    const int pass_credentials = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &pass_credentials, sizeof(pass_credentials)) == -1) {
      throw std::system_error(errno, std::generic_category(), "bridge UDS SO_PASSCRED failed");
    }
    sockaddr_un sa{};
    const socklen_t alen = detail::fill_abstract_sockaddr(addr, sa);
    if (bind(fd, reinterpret_cast<sockaddr *>(&sa), alen) == -1) {
      throw std::system_error(errno, std::generic_category(), "bridge UDS bind() failed");
    }
  } catch (...) {
    close(fd);
    throw;
  }
  return fd;
}

// Send `data`/`size` as a single datagram to `addr`.
// Retries on ECONNREFUSED/EAGAIN/EWOULDBLOCK/ENOBUFS up to
// BRIDGE_UDS_SEND_MAX_RETRIES * BRIDGE_UDS_SEND_RETRY_INTERVAL_US.
inline bool send_bridge_uds_message(
  const std::string & addr, const void * data, size_t size, const rclcpp::Logger & logger)
{
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    RCLCPP_ERROR(logger, "bridge UDS socket() failed: %s (errno: %d)", strerror(errno), errno);
    return false;
  }

  sockaddr_un sa{};
  socklen_t alen = 0;
  try {
    alen = detail::fill_abstract_sockaddr(addr, sa);
  } catch (const std::exception & e) {
    RCLCPP_ERROR(logger, "bridge UDS address invalid: %s", e.what());
    close(fd);
    return false;
  }

  const std::string display_name = (!addr.empty() && addr.front() == '\0') ? addr.substr(1) : addr;

  ssize_t send_result = -1;
  int last_errno = 0;
  for (int retry = 0; retry <= BRIDGE_UDS_SEND_MAX_RETRIES; ++retry) {
    send_result = sendto(fd, data, size, MSG_NOSIGNAL, reinterpret_cast<sockaddr *>(&sa), alen);
    if (send_result >= 0) break;
    last_errno = errno;
    if (
      last_errno != ECONNREFUSED && last_errno != EAGAIN && last_errno != EWOULDBLOCK &&
      last_errno != ENOBUFS) {
      break;
    }
    if (retry < BRIDGE_UDS_SEND_MAX_RETRIES) {
      usleep(BRIDGE_UDS_SEND_RETRY_INTERVAL_US);
    }
  }

  if (send_result < 0) {
    RCLCPP_ERROR(
      logger, "bridge UDS sendto() failed for '%s': %s (errno: %d)", display_name.c_str(),
      strerror(last_errno), last_errno);
    close(fd);
    return false;
  }
  if (static_cast<size_t>(send_result) != size) {
    // SOCK_DGRAM is atomic; a short send would indicate a kernel/API bug.
    RCLCPP_ERROR(
      logger, "bridge UDS sendto() short send to '%s': sent %zd of %zu bytes", display_name.c_str(),
      send_result, size);
    close(fd);
    return false;
  }

  close(fd);
  return true;
}

}  // namespace hermos
