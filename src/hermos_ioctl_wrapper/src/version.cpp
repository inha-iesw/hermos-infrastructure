#include "hermos_ioctl.hpp"

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
  int fd = open("/dev/hermos", O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) {
      fprintf(stderr, "%s", HERMOS_DEVICE_NOT_FOUND_MSG);
    } else {
      perror("Failed to open /dev/hermos");
    }
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
