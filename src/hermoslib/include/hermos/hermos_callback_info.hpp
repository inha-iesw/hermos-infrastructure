#pragma once

#include "hermos/hermos_epoll.hpp"
#include "hermos/hermos_epoll_update_dispatcher.hpp"
#include "hermos/hermos_smart_pointer.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>

namespace hermos
{

namespace detail
{
struct SubscriptionNotification;
}

// Capped slightly below UINT32_MAX to provide a safe margin against
// atomic wrap-around (overflow back to 0) during concurrent fetch_add calls.
constexpr uint32_t MAX_CALLBACK_INFO_ID_SAFETY_MARGIN = 1000;
constexpr uint32_t MAX_CALLBACK_INFO_ID = UINT32_MAX - MAX_CALLBACK_INFO_ID_SAFETY_MARGIN;

struct HermosExecutable;

// Base class for a type-erased object
class AnyObject
{
public:
  virtual ~AnyObject() = default;
  virtual const std::type_info & type() const = 0;
};

// Class for a specific message type
template <typename T>
class TypedMessagePtr : public AnyObject
{
  hermos::ipc_shared_ptr<T> ptr_;

public:
  explicit TypedMessagePtr(hermos::ipc_shared_ptr<T> p) : ptr_(std::move(p)) {}

  TypedMessagePtr(
    T * ptr, const std::string & topic_name, topic_local_id_t subscriber_id, int64_t entry_id,
    std::shared_ptr<detail::SubscriberRegistration> registration)
  : ptr_(ptr, topic_name, subscriber_id, entry_id, std::move(registration))
  {
  }

  const std::type_info & type() const override { return typeid(T); }

  hermos::ipc_shared_ptr<T> && get() && { return std::move(ptr_); }
};

// Type for type-erased callback function
using TypeErasedCallback = std::function<void(AnyObject &&)>;

// Type for message creator function that constructs a type-erased message
// envelope (AnyObject) from a raw shared-memory pointer and its metadata.
// Success adopts the kernel reference; on exception the caller still owns it.
using MessageCreator = std::function<std::unique_ptr<AnyObject>(
  void *, const std::string &, const topic_local_id_t, const int64_t)>;

struct CallbackInfo
{
  std::string topic_name;
  topic_local_id_t subscriber_id;
  bool is_transient_local;
  mqd_t mqdes;
  rclcpp::CallbackGroup::SharedPtr callback_group;
  TypeErasedCallback callback;
  MessageCreator message_creator;
  bool attached_once = false;
  std::shared_ptr<detail::SubscriptionNotification> notification;
  int registered_epoll_fd = -1;
  // Shared by in-flight batches so cancellation does not need the registry lock per entry.
  std::shared_ptr<std::atomic<bool>> active;
};

std::vector<std::string> get_hermos_topics_by_group(
  const rclcpp::CallbackGroup::SharedPtr & group);

// Lock ordering: when acquiring both id2_callback_info_mtx and id2_timer_info_mtx,
// always lock id2_callback_info_mtx first to avoid deadlocks.
extern std::mutex id2_callback_info_mtx;
extern std::unordered_map<uint32_t, CallbackInfo> id2_callback_info;
extern std::atomic<uint32_t> next_callback_info_id;

uint32_t allocate_callback_info_id();

template <typename T, typename Func>
TypeErasedCallback get_erased_callback(Func && callback)
{
  return [callback = std::forward<Func>(callback)](AnyObject && arg) {
    if (typeid(T) == arg.type()) {
      auto && typed_arg = static_cast<TypedMessagePtr<T> &&>(arg);
      callback(std::move(typed_arg).get());
    } else {
      RCLCPP_ERROR(
        logger, "Hermos internal implementation error: bad allocation when callback is called");
      close(hermos_fd);
      exit(EXIT_FAILURE);
    }
  };
}

template <typename MessageT, typename Func>
uint32_t register_callback(
  Func && callback, const std::string & topic_name, const topic_local_id_t subscriber_id,
  const bool is_transient_local, mqd_t mqdes, rclcpp::CallbackGroup::SharedPtr callback_group,
  std::shared_ptr<detail::SubscriberRegistration> registration = {},
  std::shared_ptr<detail::SubscriptionNotification> notification = {})
{
  // NOTE: ipc_shared_ptr<MessageT> and ipc_shared_ptr<MessageT>&& make no difference in the
  // assertion expression below, but we go with ipc_shared_ptr<MessageT>&&.
  static_assert(
    std::is_invocable_v<std::decay_t<Func>, hermos::ipc_shared_ptr<MessageT> &&> ||
      std::is_invocable_v<std::decay_t<Func>, hermos::ipc_shared_ptr<const MessageT> &&>,
    "Callback must be callable with ipc_shared_ptr<T> or ipc_shared_ptr<const T> (const&, &&, or "
    "by-value)");

  TypeErasedCallback erased_callback = get_erased_callback<MessageT>(std::forward<Func>(callback));

  auto message_creator = [registration = std::move(registration)](
                           void * ptr, const std::string & topic_name,
                           const topic_local_id_t subscriber_id, const int64_t entry_id) {
    // Construct the wrapper before adopting the kernel reference. If allocation fails,
    // receive_and_execute_message still owns and releases that reference.
    return std::make_unique<TypedMessagePtr<MessageT>>(
      static_cast<MessageT *>(ptr), topic_name, subscriber_id, entry_id, registration);
  };

  uint32_t callback_info_id = allocate_callback_info_id();
  auto active = std::make_shared<std::atomic<bool>>(true);

  {
    std::lock_guard<std::mutex> lock(id2_callback_info_mtx);
    id2_callback_info[callback_info_id] = CallbackInfo{
      topic_name,
      subscriber_id,
      is_transient_local,
      mqdes,
      std::move(callback_group),
      std::move(erased_callback),
      std::move(message_creator),
      false,
      std::move(notification),
      -1,
      std::move(active)};
  }

  EpollUpdateDispatcher::get_instance().request_update_all();

  return callback_info_id;
}

/**
 * @brief Stop callback delivery and detach its notification before releasing the registry entry.
 *
 * In-flight handlers may retain the notification descriptor after removal. Detaching
 * it here prevents an unread message from repeatedly waking the former executor.
 * @param callback_info_id Registry entry to remove; an absent entry is ignored.
 * @return Nothing.
 * @throws std::runtime_error if the epoll registration cannot be detached.
 */
void unregister_callback(uint32_t callback_info_id);

void receive_and_execute_message(
  uint32_t callback_info_id, pid_t my_pid, const CallbackInfo & callback_info,
  std::mutex & ready_hermos_executables_mutex,
  std::vector<HermosExecutable> & ready_hermos_executables);

void enqueue_receive_and_execute(
  uint32_t callback_info_id, pid_t my_pid, const CallbackInfo & callback_info,
  std::mutex & ready_hermos_executables_mutex,
  std::vector<HermosExecutable> & ready_hermos_executables);

class SubscriptionEventHandler : public EpollEventHandler
{
  int epoll_fd_ = -1;
  pid_t my_pid_;
  std::mutex * ready_hermos_executables_mutex_;
  std::vector<HermosExecutable> * ready_hermos_executables_;

public:
  SubscriptionEventHandler(
    const pid_t my_pid, std::mutex * ready_hermos_executables_mutex,
    std::vector<HermosExecutable> * ready_hermos_executables)
  : my_pid_(my_pid),
    ready_hermos_executables_mutex_(ready_hermos_executables_mutex),
    ready_hermos_executables_(ready_hermos_executables)
  {
  }

  ~SubscriptionEventHandler() override;

  [[nodiscard]] EpollEventType get_type() const override { return EpollEventType::Subscription; }

  void prepare_epoll(int epoll_fd, const CallbackGroupValidator & validate_callback_group) override;

  void handle(EpollEventLocalID event_local_id) override;
};

}  // namespace hermos
