// Copyright 2026
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

// Match the native runtime: an explicit selection must never fall back to another module.
inline const char * hermos_device_path()
{
  const char * selected = std::getenv("HERMOS_DEVICE_PATH");
  return selected && *selected ? selected : "/dev/hermos";
}

// Preserve errno across diagnostics so callers can return the original open failure.
inline int open_hermos_device()
{
  const char * path = hermos_device_path();
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    const int error = errno;
    fprintf(stderr, "Failed to open %s: %s\n", path, std::strerror(error));
    if (error == ENOENT) {
      fprintf(stderr, "Ensure the kernel module is installed (sudo modprobe hermos for /dev/hermos) "
        "and HERMOS_DEVICE_PATH selects its device.\n");
    }
    errno = error;
  }
  return fd;
}
