#include "hermos_ioctl.hpp"
#include "device/path.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {

char * get_hermos_kmod_version()
{
  int fd = open_hermos_device();
  if (fd < 0) {
    return nullptr;
  }

  struct ioctl_get_version_args args = {};
  if (ioctl(fd, HERMOS_GET_VERSION_CMD, &args) < 0) {
    perror("Failed to get hermos kmod version");
    close(fd);
    return nullptr;
  }

  close(fd);

  char * version = static_cast<char *>(malloc(VERSION_BUFFER_LEN));
  if (version == nullptr) {
    return nullptr;
  }
  strncpy(version, args.ret_version, VERSION_BUFFER_LEN);
  version[VERSION_BUFFER_LEN - 1] = '\0';
  return version;
}

void free_hermos_kmod_version(char * version)
{
  free(version);
}
}
