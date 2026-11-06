#include "hermos_ioctl.hpp"
#include "device/path.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {

struct topic_info_ret * get_hermos_sub_nodes(
  const char * topic_name, int * topic_info_ret_count, uint32_t domain_id)
{
  *topic_info_ret_count = 0;

  int fd = open_hermos_device();
  if (fd < 0) {
    return nullptr;
  }

  struct topic_info_ret * hermos_topic_info_ret_buffer = static_cast<struct topic_info_ret *>(
    malloc(MAX_TOPIC_INFO_RET_NUM * sizeof(struct topic_info_ret)));

  if (hermos_topic_info_ret_buffer == nullptr) {
    fprintf(stderr, "Memory allocation failed\n");
    close(fd);
    return nullptr;
  }

  union ioctl_topic_info_args topic_info_args = {};
  topic_info_args.topic_name = {topic_name, strlen(topic_name)};
  topic_info_args.topic_info_ret_buffer_addr =
    reinterpret_cast<uint64_t>(hermos_topic_info_ret_buffer);
  topic_info_args.topic_info_ret_buffer_size = MAX_TOPIC_INFO_RET_NUM;
  topic_info_args.domain_id = domain_id;
  if (ioctl(fd, HERMOS_GET_TOPIC_SUBSCRIBER_INFO_CMD, &topic_info_args) < 0) {
    perror("HERMOS_GET_TOPIC_SUBSCRIBER_INFO_CMD failed");
    free(hermos_topic_info_ret_buffer);
    close(fd);
    return nullptr;
  }

  if (topic_info_args.ret_topic_info_ret_num == 0) {
    free(hermos_topic_info_ret_buffer);
    close(fd);
    return nullptr;
  }

  *topic_info_ret_count = static_cast<int>(topic_info_args.ret_topic_info_ret_num);
  close(fd);
  return hermos_topic_info_ret_buffer;
}

struct topic_info_ret * get_hermos_pub_nodes(
  const char * topic_name, int * topic_info_ret_count, uint32_t domain_id)
{
  *topic_info_ret_count = 0;

  int fd = open_hermos_device();
  if (fd < 0) {
    return nullptr;
  }

  struct topic_info_ret * hermos_topic_info_ret_buffer = static_cast<struct topic_info_ret *>(
    malloc(MAX_TOPIC_INFO_RET_NUM * sizeof(struct topic_info_ret)));

  if (hermos_topic_info_ret_buffer == nullptr) {
    fprintf(stderr, "Memory allocation failed\n");
    close(fd);
    return nullptr;
  }

  union ioctl_topic_info_args topic_info_args = {};
  topic_info_args.topic_name = {topic_name, strlen(topic_name)};
  topic_info_args.topic_info_ret_buffer_addr =
    reinterpret_cast<uint64_t>(hermos_topic_info_ret_buffer);
  topic_info_args.topic_info_ret_buffer_size = MAX_TOPIC_INFO_RET_NUM;
  topic_info_args.domain_id = domain_id;
  if (ioctl(fd, HERMOS_GET_TOPIC_PUBLISHER_INFO_CMD, &topic_info_args) < 0) {
    perror("HERMOS_GET_TOPIC_PUBLISHER_INFO_CMD failed");
    free(hermos_topic_info_ret_buffer);
    close(fd);
    return nullptr;
  }

  if (topic_info_args.ret_topic_info_ret_num == 0) {
    free(hermos_topic_info_ret_buffer);
    close(fd);
    return nullptr;
  }

  *topic_info_ret_count = static_cast<int>(topic_info_args.ret_topic_info_ret_num);
  close(fd);
  return hermos_topic_info_ret_buffer;
}

void free_hermos_topic_info_ret(struct topic_info_ret * array)
{
  free(array);
}
}
