#include "hermos/hermos_callback_info.hpp"

#include "hermos/hermos_epoll_event.hpp"
#include "hermos/hermos_executor.hpp"
#include "hermos/hermos_mq.hpp"
#include "hermos/hermos_subscription.hpp"
#include "hermos/hermos_tracepoint_wrapper.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <array>
#include <stdexcept>
#include <utility>

namespace hermos
{

std::mutex id2_callback_info_mtx;
const int callback_map_bkt_cnt = 100;  // arbitrary size to prevent rehash
std::unordered_map<uint32_t, CallbackInfo> id2_callback_info(callback_map_bkt_cnt);
std::atomic<uint32_t> next_callback_info_id;

uint32_t allocate_callback_info_id()
{
  const uint32_t callback_info_id = next_callback_info_id.fetch_add(1);
  if (callback_info_id >= MAX_CALLBACK_INFO_ID) {
    throw std::runtime_error("Callback info ID overflow: too many callbacks registered");
  }
  return callback_info_id;
}

void unregister_callback(uint32_t callback_info_id)
{
  std::lock_guard<std::mutex> lock(id2_callback_info_mtx);
  const auto it = id2_callback_info.find(callback_info_id);
  if (it == id2_callback_info.end()) {
    return;
  }
  auto & info = it->second;
  info.active->store(false, std::memory_order_release);
  update_epoll_registration(
    info.registered_epoll_fd, -1, info.mqdes, EpollEventType::Subscription, callback_info_id);
  id2_callback_info.erase(it);
}

void receive_and_execute_message(
  const uint32_t callback_info_id, const pid_t my_pid, const CallbackInfo & callback_info,
  std::mutex & ready_hermos_executables_mutex,
  std::vector<HermosExecutable> & ready_hermos_executables)
{
  std::array<publisher_shm_info, MAX_PUBLISHER_NUM> pub_shm_infos{};

  union ioctl_receive_msg_args receive_args = {};
  receive_args.topic_name = {callback_info.topic_name.c_str(), callback_info.topic_name.size()};
  receive_args.subscriber_id = callback_info.subscriber_id;
  receive_args.pub_shm_info_addr = reinterpret_cast<uint64_t>(pub_shm_infos.data());
  receive_args.pub_shm_info_size = MAX_PUBLISHER_NUM;

  // The ioctl owns references before mapping or message-wrapper allocation can fail.
  struct UnadoptedEntries
  {
    const CallbackInfo & info;
    const ioctl_receive_msg_args & args;
    uint16_t adopted = 0;
    bool received = false;
    ~UnadoptedEntries()
    {
      if (!received) {
        return;
      }
      for (uint16_t i = adopted; i < args.ret_entry_num; ++i) {
        release_subscriber_reference(info.topic_name, info.subscriber_id, args.ret_entry_ids[i]);
      }
    }
  } unadopted{callback_info, receive_args};

  {
    std::lock_guard<std::mutex> lock(mmap_mtx);

    if (ioctl(hermos_fd, HERMOS_RECEIVE_MSG_CMD, &receive_args) < 0) {
      RCLCPP_ERROR(logger, "HERMOS_RECEIVE_MSG_CMD failed: %s", strerror(errno));
      close(hermos_fd);
      exit(EXIT_FAILURE);
    }
    unadopted.received = true;

    // Map the shared memory region with read permissions whenever a new publisher is discovered.
    for (uint32_t i = 0; i < receive_args.ret_pub_shm_num; i++) {
      const pid_t pid = pub_shm_infos[i].pid;
      const uint64_t addr = pub_shm_infos[i].shm_addr;
      const uint64_t size = pub_shm_infos[i].shm_size;
      map_read_only_area(pid, addr, size);
    }
  }

  // Adopt the entire batch before calling user code. A throwing callback or a
  // callback destroying its subscription must still release the other entries.
  std::array<std::unique_ptr<AnyObject>, MAX_RECEIVE_NUM> messages;

  for (; unadopted.adopted < receive_args.ret_entry_num; ++unadopted.adopted) {
    const auto i = unadopted.adopted;
    messages[i] = callback_info.message_creator(
      reinterpret_cast<void *>(receive_args.ret_entry_addrs[i]), callback_info.topic_name,
      callback_info.subscriber_id, receive_args.ret_entry_ids[i]);
  }

  for (uint16_t i = 0; i < receive_args.ret_entry_num; ++i) {
    if (!callback_info.active->load(std::memory_order_acquire)) {
      break;
    }
    const int64_t entry_id = receive_args.ret_entry_ids[i];
    const void * callback_addr = &messages[i];  // For CARET

    {
      constexpr uint8_t PID_SHIFT_BITS = 32;
      uint64_t pid_callback_info_id =
        (static_cast<uint64_t>(my_pid) << PID_SHIFT_BITS) | callback_info_id;
      // NOTE: The hermos_create_callable tracepoint was previously used to associate
      // pid_callback_info_id with callable_addr, as well as to associate callable with entry_addr
      // and entry_id. In the current implementation, however, this information can be obtained when
      // the callback starts and ends, rendering this tracepoint unnecessary. Nevertheless, since
      // the current implementation may change in the future, we are retaining this tracepoint to
      // ensure that CARET can be used without modifying its implementation.
      TRACEPOINT(hermos_create_callable, callback_addr, entry_id, pid_callback_info_id);
    }

    // NOTE: hermos_callable_start should be renamed to hermos_callback_start. As mentioned
    // earlier, we will not change the name in order to avoid requiring changes to be made to the
    // implementation of CARET.
    TRACEPOINT(hermos_callable_start, callback_addr);
    callback_info.callback(std::move(*messages[i]));
    // NOTE: hermos_callable_end should be renamed to hermos_callback_end. As mentioned earlier,
    // we will not change the name in order to avoid requiring changes to be made to the
    // implementation of CARET.
    TRACEPOINT(hermos_callable_end, callback_addr);
    messages[i].reset();
  }

  // If more entries remain, re-enqueue to allow other callbacks to be scheduled in between.
  if (receive_args.ret_call_again) {
    std::lock_guard<std::mutex> lock(id2_callback_info_mtx);
    if (id2_callback_info.find(callback_info_id) == id2_callback_info.end()) {
      return;
    }
    enqueue_receive_and_execute(
      callback_info_id, my_pid, callback_info, ready_hermos_executables_mutex,
      ready_hermos_executables);
  }
}

void enqueue_receive_and_execute(
  const uint32_t callback_info_id, const pid_t my_pid, const CallbackInfo & callback_info,
  std::mutex & ready_hermos_executables_mutex,
  std::vector<HermosExecutable> & ready_hermos_executables)
{
  auto deferred_callable = std::make_shared<std::function<void()>>(
    [callback_info_id, my_pid, &ready_hermos_executables_mutex,
     &ready_hermos_executables]() {
      CallbackInfo current_info;
      {
        std::lock_guard<std::mutex> lock(id2_callback_info_mtx);
        const auto it = id2_callback_info.find(callback_info_id);
        if (it == id2_callback_info.end()) {
          return;
        }
        current_info = it->second;
      }
      receive_and_execute_message(
        callback_info_id, my_pid, current_info, ready_hermos_executables_mutex,
        ready_hermos_executables);
    });

  std::lock_guard<std::mutex> ready_lock{ready_hermos_executables_mutex};
  ready_hermos_executables.emplace_back(
    HermosExecutable{deferred_callable, callback_info.callback_group});
}

std::vector<std::string> get_hermos_topics_by_group(
  const rclcpp::CallbackGroup::SharedPtr & group)
{
  std::vector<std::string> topic_names;

  {
    std::lock_guard<std::mutex> lock(id2_callback_info_mtx);
    for (const auto & [id, callback_info] : id2_callback_info) {
      if (callback_info.callback_group == group) {
        topic_names.push_back(callback_info.topic_name);
      }
    }
  }

  std::sort(topic_names.begin(), topic_names.end());

  return topic_names;
}

SubscriptionEventHandler::~SubscriptionEventHandler()
{
  std::lock_guard<std::mutex> lock(id2_callback_info_mtx);
  for (auto & [id, info] : id2_callback_info) {
    if (info.registered_epoll_fd == epoll_fd_) {
      info.registered_epoll_fd = -1;
    }
  }
}

void SubscriptionEventHandler::prepare_epoll(
  int epoll_fd, const CallbackGroupValidator & validate_callback_group)
{
  std::lock_guard<std::mutex> lock(id2_callback_info_mtx);
  epoll_fd_ = epoll_fd;

  for (auto & it : id2_callback_info) {
    const uint32_t callback_info_id = it.first;
    CallbackInfo & callback_info = it.second;

    if (callback_info.mqdes < 0) {
      continue;
    }
    if (!validate_callback_group(callback_info.callback_group)) {
      if (callback_info.registered_epoll_fd == epoll_fd) {
        update_epoll_registration(
          callback_info.registered_epoll_fd, -1, callback_info.mqdes,
          EpollEventType::Subscription, callback_info_id);
      }
      continue;
    }
    if (callback_info.registered_epoll_fd == epoll_fd) {
      continue;
    }
    update_epoll_registration(
      callback_info.registered_epoll_fd, epoll_fd, callback_info.mqdes,
      EpollEventType::Subscription, callback_info_id);

    // A previous executor may have consumed the notification but still hold
    // the deferred receive in its queue. Resume that receive after transfer.
    if (callback_info.is_transient_local || callback_info.attached_once) {
      hermos::enqueue_receive_and_execute(
        callback_info_id, my_pid_, callback_info, *ready_hermos_executables_mutex_,
        *ready_hermos_executables_);
    }

    callback_info.attached_once = true;
  }
}

void SubscriptionEventHandler::handle(EpollEventLocalID event_local_id)
{
  // Subscription callback event
  const uint32_t callback_info_id = event_local_id;
  CallbackInfo callback_info;
  MqMsgHermos mq_msg = {};
  ssize_t ret;

  {
    std::lock_guard<std::mutex> lock(id2_callback_info_mtx);

    const auto it = id2_callback_info.find(callback_info_id);
    if (it == id2_callback_info.end() || it->second.registered_epoll_fd != epoll_fd_) {
      // Callback was unregistered (subscription destroyed) - this is normal
      return;
    }

    callback_info = it->second;
    // Serialize the nonblocking read with attachment transfer. An event
    // returned by the old epoll must not drain the new owner's notification.
    if (callback_info.notification && callback_info.notification->is_driver_fd()) {
      uint64_t wake;
      do {
        ret = read(callback_info.mqdes, &wake, sizeof(wake));
      } while (ret < 0 && errno == EINTR);
      if (ret == 0) {
        // The endpoint was removed. EPOLLHUP remains ready until the fd is detached.
        update_epoll_registration(
          it->second.registered_epoll_fd, -1, callback_info.mqdes,
          EpollEventType::Subscription, callback_info_id);
        it->second.mqdes = -1;
        return;
      }
    } else {
      ret = mq_receive(
        callback_info.mqdes, reinterpret_cast<char *>(&mq_msg), sizeof(mq_msg), nullptr);
    }
  }
  if (ret < 0) {
    if (errno != EAGAIN) {
      RCLCPP_ERROR_STREAM(
        logger, "Notification read failed for topic '"
                  << callback_info.topic_name << "' (subscriber_id=" << callback_info.subscriber_id
                  << "): " << strerror(errno));
      close(hermos_fd);
      exit(EXIT_FAILURE);
    }
    return;
  }

  hermos::enqueue_receive_and_execute(
    callback_info_id, my_pid_, callback_info, *ready_hermos_executables_mutex_,
    *ready_hermos_executables_);
}

}  // namespace hermos
