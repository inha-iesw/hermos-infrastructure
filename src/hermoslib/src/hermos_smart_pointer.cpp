#include "hermos/hermos_smart_pointer.hpp"

namespace hermos
{

void release_subscriber_reference(
  const std::string & topic_name, const topic_local_id_t pubsub_id, const int64_t entry_id)
{
  struct ioctl_update_entry_args entry_args = {};
  entry_args.topic_name = {topic_name.c_str(), topic_name.size()};
  entry_args.pubsub_id = pubsub_id;
  entry_args.entry_id = entry_id;
  if (ioctl(hermos_fd, HERMOS_RELEASE_SUB_REF_CMD, &entry_args) < 0) {
    RCLCPP_ERROR(logger, "HERMOS_RELEASE_SUB_REF_CMD failed: %s", strerror(errno));
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }
}

}  // namespace hermos
