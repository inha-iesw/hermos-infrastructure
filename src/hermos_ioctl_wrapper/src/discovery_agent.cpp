#include "hermos_ioctl.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>

namespace
{
// Open /dev/hermos for the discovery-agent ioctls, or return -1 after printing why.
int open_hermos_device()
{
  int fd = open("/dev/hermos", O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) {
      fprintf(stderr, "%s", HERMOS_DEVICE_NOT_FOUND_MSG);
    } else {
      perror("Failed to open /dev/hermos");
    }
  }
  return fd;
}
}  // namespace

extern "C" {

// Claim the singleton for this (IPC namespace, domain). Returns 0 if this caller won the claim,
// 1 if an agent is already registered (caller must exit), or -1 on error (errno set).
int hermos_discovery_agent_register(uint32_t domain_id)
{
  int fd = open_hermos_device();
  if (fd < 0) return -1;

  struct ioctl_add_discovery_agent_args args = {};
  args.domain_id = domain_id;
  if (ioctl(fd, HERMOS_ADD_DISCOVERY_AGENT_CMD, &args) < 0) {
    perror("HERMOS_ADD_DISCOVERY_AGENT_CMD failed");
    close(fd);
    return -1;
  }

  close(fd);
  return args.ret_already_exists ? 1 : 0;
}

// Read-only idle poll (the agent counts consecutive idle polls before exiting). Returns 1 (domain
// empty), 0 (busy), -1 on error.
int hermos_discovery_agent_should_exit(uint32_t domain_id)
{
  int fd = open_hermos_device();
  if (fd < 0) return -1;

  struct ioctl_discovery_agent_should_exit_args args = {};
  args.domain_id = domain_id;
  args.commit = false;
  if (ioctl(fd, HERMOS_DISCOVERY_AGENT_SHOULD_EXIT_CMD, &args) < 0) {
    perror("HERMOS_DISCOVERY_AGENT_SHOULD_EXIT_CMD failed");
    close(fd);
    return -1;
  }

  close(fd);
  return args.ret_should_exit ? 1 : 0;
}

// Atomic exit gate: deregister iff the domain is still empty. Returns 1 (deregistered, safe to
// exit), 0 (a process raced in, keep running), or -1 on error.
int hermos_discovery_agent_commit_exit(uint32_t domain_id)
{
  int fd = open_hermos_device();
  if (fd < 0) return -1;

  struct ioctl_discovery_agent_should_exit_args args = {};
  args.domain_id = domain_id;
  args.commit = true;
  if (ioctl(fd, HERMOS_DISCOVERY_AGENT_SHOULD_EXIT_CMD, &args) < 0) {
    perror("HERMOS_DISCOVERY_AGENT_SHOULD_EXIT_CMD failed");
    close(fd);
    return -1;
  }

  close(fd);
  return args.ret_should_exit ? 1 : 0;
}

// Read-only liveness query for the CLI status verb. Returns 1 (an agent is registered), 0 (none),
// or -1 on error.
int hermos_discovery_agent_exists(uint32_t domain_id)
{
  int fd = open_hermos_device();
  if (fd < 0) return -1;

  struct ioctl_discovery_agent_exists_args args = {};
  args.domain_id = domain_id;
  if (ioctl(fd, HERMOS_DISCOVERY_AGENT_EXISTS_CMD, &args) < 0) {
    perror("HERMOS_DISCOVERY_AGENT_EXISTS_CMD failed");
    close(fd);
    return -1;
  }

  close(fd);
  return args.ret_exists ? 1 : 0;
}
}
