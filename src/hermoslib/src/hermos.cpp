#include "hermos/hermos.hpp"
#include "ipc/access.hpp"

#include "hermos/hermos_ioctl.hpp"
#include "hermos/hermos_mq.hpp"
#include "hermos/hermos_version.hpp"
#include "hermos/bridge/performance/hermos_performance_bridge_manager.hpp"

#include <dlfcn.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

extern "C" {
const char * hermos_get_version()
{
  return hermoslib::VERSION;
}
}

namespace hermos
{

int hermos_fd = -1;
std::vector<int> shm_fds;
std::mutex shm_fds_mtx;
std::mutex mmap_mtx;
// mmap_mtx: Prevents a race condition and segfault between two threads
// in a multithreaded executor using the same mqueue_fd.
//
// Race Scenario:
// 1. Thread 1 (T1):
//    - Calls epoll_wait(), mq_receive(), then ioctl(RECEIVE_CMD), initially obtaining
//      publisher info (PID, shared memory address `shm_addr`).
//    - Critical: OS context switch occurs *after* ioctl() but *before* T1 fully
//      processes/maps `shm_addr`.
// 2. Thread 2 (T2):
//    - Calls epoll_wait(), mq_receive(), then ioctl(RECEIVE_CMD) on the same mqueue_fd,
//      but does *not* receive publisher info (assuming it's already set up).
//    - Proceeds to a callback which attempts to use `shm_addr`, leading to a SEGFAULT.
//
// Root Cause: T2's callback uses `shm_addr` that T1 fetched but hadn't initialized/mapped yet.
// This mutex ensures atomicity for T1's critical section: from ioctl fetching publisher
// info through to completing shared memory setup.

namespace
{
// Every process maps the heaps of the kmod's fixed slots [start, start + num * size) at the
// same addresses. A mapping another library places in that range (observed: 1 GiB anonymous
// mappings created during CUDA start-up) makes a later MAP_FIXED_NOREPLACE heap mapping fail
// with EEXIST. initialize_hermos therefore reserves the whole range PROT_NONE before other
// libraries run, and heaps replace their own slot of the reservation with MAP_FIXED.
// Written once during single-threaded initialization (inherited by fork), then only read.
uintptr_t reserved_begin = 0;
uintptr_t reserved_end = 0;
uint64_t reserved_slot_size = 0;
// MAP_FIXED replaces silently, so one bit per slot keeps the EEXIST guard against mapping
// a slot twice. Slot counts above the bitmap leave the range unreserved.
constexpr uint64_t MAX_RESERVED_SLOTS = 65536;
std::atomic<uint64_t> mapped_slots[MAX_RESERVED_SLOTS / 64];

// No heap allocation: this runs before the heaphook's allocator exists.
bool read_module_parameter(const char * module, const char * name, unsigned long long & value)
{
  char path[256];
  const int path_length =
    snprintf(path, sizeof(path), "/sys/module/%s/parameters/%s", module, name);
  if (path_length <= 0 || static_cast<size_t>(path_length) >= sizeof(path)) {
    return false;
  }
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  char text[32];
  const ssize_t length = read(fd, text, sizeof(text) - 1);
  close(fd);
  if (length <= 0) {
    return false;
  }
  text[length] = '\0';
  char * end = nullptr;
  errno = 0;
  value = strtoull(text, &end, 10);
  return errno == 0 && end != text && (*end == '\n' || *end == '\0');
}

void warn_heap_range_unreserved(const char * reason)
{
  char message[256];
  const int length = snprintf(
    message, sizeof(message),
    "[WARN] [Hermos]: heap slot range not reserved (%s); foreign mappings can block heap slots\n",
    reason);
  if (length > 0) {
    const size_t size = std::min(static_cast<size_t>(length), sizeof(message) - 1);
    if (write(STDERR_FILENO, message, size) < 0) {
      // Best-effort diagnostic; nothing else can report an unwritable stderr.
    }
  }
}

// The kmod's character device is named after the module (KBUILD_MODNAME), whose parameters
// give the slot range. Failure keeps the unreserved behaviour and says so once.
void reserve_heap_range(const char * device_path)
{
  const char * slash = strrchr(device_path, '/');
  const char * module = slash != nullptr ? slash + 1 : device_path;
  unsigned long long start = 0;
  unsigned long long num = 0;
  unsigned long long size_gb = 0;
  if (
    !read_module_parameter(module, "mempool_start_addr", start) ||
    !read_module_parameter(module, "mempool_num", num) ||
    !read_module_parameter(module, "mempool_size_gb", size_gb)) {
    warn_heap_range_unreserved("kmod parameters unreadable");
    return;
  }
  if (num == 0 || num > MAX_RESERVED_SLOTS || size_gb == 0 || size_gb > (UINT64_MAX >> 30) / num) {
    warn_heap_range_unreserved("unsupported slot count or size");
    return;
  }
  const uint64_t slot_size = static_cast<uint64_t>(size_gb) << 30;
  const uint64_t length = static_cast<uint64_t>(num) * slot_size;
  if (start == 0 || start > UINTPTR_MAX - length) {
    warn_heap_range_unreserved("slot range outside the address space");
    return;
  }
  void * reservation = mmap(
    reinterpret_cast<void *>(start), length, PROT_NONE,
    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
  if (reservation == MAP_FAILED) {
    warn_heap_range_unreserved(strerror(errno));
    return;
  }
  if (reservation != reinterpret_cast<void *>(start)) {
    // A kernel without MAP_FIXED_NOREPLACE treats the address as a hint only.
    munmap(reservation, length);
    warn_heap_range_unreserved("fixed reservation unsupported");
    return;
  }
  reserved_begin = start;
  reserved_end = start + length;
  reserved_slot_size = slot_size;
}

// Returns the bitmap word and bit of a whole reserved slot, or nullptr outside the reservation.
std::atomic<uint64_t> * reserved_slot(const uint64_t addr, const uint64_t size, uint64_t & bit)
{
  if (
    reserved_end == 0 || size != reserved_slot_size || addr < reserved_begin ||
    addr >= reserved_end || (addr - reserved_begin) % reserved_slot_size != 0) {
    return nullptr;
  }
  const uint64_t index = (addr - reserved_begin) / reserved_slot_size;
  bit = uint64_t{1} << (index % 64);
  return &mapped_slots[index / 64];
}
}  // namespace

void * map_area(
  const pid_t pid, const uint64_t shm_addr, const uint64_t shm_size, const bool writable)
{
  const std::string shm_name = create_shm_name(pid);

  int oflag = (writable ? O_CREAT | O_EXCL | O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC;
  const int shm_mode = 0600;
  int shm_fd = shm_open(shm_name.c_str(), oflag, shm_mode);
  if (shm_fd == -1) {
    RCLCPP_ERROR(logger, "shm_open failed: %s", strerror(errno));
    close(hermos_fd);
    return nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(shm_fds_mtx);
    shm_fds.push_back(shm_fd);
  }

  auto cleanup_shm_fd = [&]() {
    {
      std::lock_guard<std::mutex> lock(shm_fds_mtx);
      shm_fds.erase(std::remove(shm_fds.begin(), shm_fds.end(), shm_fd), shm_fds.end());
    }
    close(shm_fd);
    if (writable) {
      shm_unlink(shm_name.c_str());
    }
  };

  if (writable) {
    if (ftruncate(shm_fd, static_cast<off_t>(shm_size)) == -1) {
      RCLCPP_ERROR(logger, "ftruncate failed: %s", strerror(errno));
      cleanup_shm_fd();
      close(hermos_fd);
      return nullptr;
    }

    const int new_shm_mode = 0400;
    if (fchmod(shm_fd, new_shm_mode) == -1) {
      RCLCPP_ERROR(logger, "fchmod failed: %s", strerror(errno));
      cleanup_shm_fd();
      close(hermos_fd);
      return nullptr;
    }
  }

  if (!detail::validate_private_heap(shm_fd, shm_size)) {
    RCLCPP_ERROR(logger, "Rejected shared heap ownership, permissions or size: %s", strerror(errno));
    cleanup_shm_fd();
    close(hermos_fd);
    return nullptr;
  }

  int prot = writable ? PROT_READ | PROT_WRITE : PROT_READ;
  uint64_t slot_bit = 0;
  std::atomic<uint64_t> * slot_word = reserved_slot(shm_addr, shm_size, slot_bit);
  void * ret = MAP_FAILED;
  if (slot_word == nullptr) {
    ret = mmap(
      reinterpret_cast<void *>(shm_addr), shm_size, prot, MAP_SHARED | MAP_FIXED_NOREPLACE, shm_fd,
      0);
  } else if ((slot_word->fetch_or(slot_bit) & slot_bit) != 0) {
    errno = EEXIST;  // already holds a heap; same failure as MAP_FIXED_NOREPLACE would give
  } else {
    ret = mmap(
      reinterpret_cast<void *>(shm_addr), shm_size, prot, MAP_SHARED | MAP_FIXED, shm_fd, 0);
  }

  if (ret == MAP_FAILED) {
    RCLCPP_ERROR(logger, "mmap failed: %s", strerror(errno));
    cleanup_shm_fd();
    close(hermos_fd);
    return nullptr;
  }

  return ret;
}

void * map_writable_area(const pid_t pid, const uint64_t shm_addr, const uint64_t shm_size)
{
  return map_area(pid, shm_addr, shm_size, true);
}

void map_read_only_area(const pid_t pid, const uint64_t shm_addr, const uint64_t shm_size)
{
  if (map_area(pid, shm_addr, shm_size, false) == nullptr) {
    exit(EXIT_FAILURE);
  }
}

// Initializes the child allocator for bridge functionality.
// Note: This function must only be called in a forked child process before TLSF initialization.
// Calling it after initialization will result in double initialization.
void initialize_bridge_allocator(void * mempool_ptr, size_t mempool_size)
{
  void * handle = dlopen(nullptr, RTLD_NOW);
  if (handle == nullptr) {
    const char * err_msg = dlerror();
    throw std::runtime_error(
      std::string("dlopen failed: ") + (err_msg != nullptr ? err_msg : "Unknown"));
  }

  using InitFunc = bool (*)(void *, size_t);
  auto init_func = reinterpret_cast<InitFunc>(dlsym(handle, "init_child_allocator"));

  const char * dlsym_error = dlerror();
  if ((dlsym_error != nullptr) || (init_func == nullptr)) {
    dlclose(handle);
    throw std::runtime_error(
      std::string("dlsym 'init_child_allocator' failed: ") +
      (dlsym_error != nullptr ? dlsym_error : "Symbol is null"));
  }

  bool success = init_func(mempool_ptr, mempool_size);

  if (!success) {
    throw std::runtime_error("init_child_allocator returned false.");
  }
}

initialize_hermos_result acquire_hermos_resources_for_bridge()
{
  union ioctl_add_process_args add_process_args = {};
  add_process_args.is_performance_bridge_manager = true;
  add_process_args.domain_id = get_ros_domain_id();
  if (ioctl(hermos_fd, HERMOS_ADD_PROCESS_CMD, &add_process_args) < 0) {
    throw std::runtime_error(std::string("HERMOS_ADD_PROCESS_CMD failed: ") + strerror(errno));
  }

  if (add_process_args.ret_performance_bridge_daemon_exist) {
    close(hermos_fd);
    exit(EXIT_SUCCESS);
  }

  void * mempool_ptr =
    map_writable_area(getpid(), add_process_args.ret_addr, add_process_args.ret_shm_size);

  if (mempool_ptr == nullptr) {
    throw std::runtime_error("map_writable_area failed.");
  }

  return {
    mempool_ptr,
    add_process_args.ret_shm_size,
  };
}

void poll_for_unlink()
{
  struct ioctl_add_unlink_daemon_args add_daemon_args = {};
  if (ioctl(hermos_fd, HERMOS_ADD_UNLINK_DAEMON_CMD, &add_daemon_args) < 0) {
    RCLCPP_ERROR(logger, "HERMOS_ADD_UNLINK_DAEMON_CMD failed: %s", strerror(errno));
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }
  if (add_daemon_args.ret_already_exists) return;

  std::vector<exit_subscription_mq_info> mq_info_buf(MAX_SUBSCRIPTION_NUM_PER_PROCESS);

  while (true) {
    sleep(1);

    struct ioctl_get_exit_process_args get_exit_process_args = {};
    do {
      get_exit_process_args = {};
      get_exit_process_args.subscription_mq_info_buffer_addr =
        reinterpret_cast<uint64_t>(mq_info_buf.data());
      get_exit_process_args.subscription_mq_info_buffer_size =
        static_cast<uint32_t>(mq_info_buf.size());
      if (ioctl(hermos_fd, HERMOS_GET_EXIT_PROCESS_CMD, &get_exit_process_args) < 0) {
        RCLCPP_ERROR(logger, "HERMOS_GET_EXIT_PROCESS_CMD failed: %s", strerror(errno));
        close(hermos_fd);
        exit(EXIT_FAILURE);
      }

      if (get_exit_process_args.ret_pid > 0) {
        const std::string shm_name = create_shm_name(get_exit_process_args.ret_pid);
        shm_unlink(shm_name.c_str());

        // Unlink subscription MQs that the exited process owned
        for (uint32_t i = 0; i < get_exit_process_args.ret_subscription_mq_info_num; i++) {
          // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
          const std::string topic_name(mq_info_buf[i].topic_name);
          const std::string sub_mq_name =
            create_mq_name_for_hermos_publish(topic_name, mq_info_buf[i].subscriber_id);
          mq_unlink(sub_mq_name.c_str());
        }
      }
    } while (get_exit_process_args.ret_pid > 0);

    if (get_exit_process_args.ret_daemon_should_exit) {
      struct ioctl_unlink_daemon_should_exit_args should_exit_args = {};
      if (ioctl(hermos_fd, HERMOS_UNLINK_DAEMON_SHOULD_EXIT_CMD, &should_exit_args) < 0) {
        RCLCPP_ERROR(
          logger, "HERMOS_UNLINK_DAEMON_SHOULD_EXIT_CMD failed: %s", strerror(errno));
        close(hermos_fd);
        exit(EXIT_FAILURE);
      }
      if (should_exit_args.ret_should_exit) break;
    }
  }

  exit(0);
}

void poll_for_bridge_manager()
{
  try {
    const auto resources = acquire_hermos_resources_for_bridge();
    initialize_bridge_allocator(resources.mempool_ptr, resources.mempool_size);
    PerformanceBridgeManager manager;
    manager.run();
  } catch (const std::exception & e) {
    RCLCPP_ERROR(logger, "BridgeManager crashed: %s", e.what());
    exit(EXIT_FAILURE);
  }
  exit(0);
}

// Forked into the pre-allocator child: must not allocate. execlp() with a literal
// argv is malloc-free, unlike setenv() which may allocate before hermos's TLSF
// allocator is ready.
void exec_discovery_agent()
{
  execlp(
    "ros2", "ros2", "run", "ros2hermos_discovery_agent", "discovery_agent", "--exit-when-idle",
    static_cast<char *>(nullptr));
  // execlp only returns on failure. This still runs in the pre-allocator forked child, so the
  // failure path must be async-signal-safe and allocation-free: RCLCPP_ERROR / strerror / exit()
  // may allocate or run atexit handlers before the TLSF allocator is ready. Use write() + _exit().
  constexpr std::string_view err_msg = "[ERROR] [Hermos] Failed to exec the discovery agent\n";
  // Best-effort diagnostic -- we _exit next regardless; the assignment + cast just
  // consume write()'s warn_unused_result without allocating or logging.
  const ssize_t written = write(STDERR_FILENO, err_msg.data(), err_msg.size());
  static_cast<void>(written);
  _exit(EXIT_FAILURE);
}

struct semver
{
  int major;
  int minor;
  int patch;
};

bool parse_semver(const char * version, struct semver * out_ver)
{
  if (version == nullptr || out_ver == nullptr) {
    return false;
  }

  out_ver->major = 0;
  out_ver->minor = 0;
  out_ver->patch = 0;

  std::string version_str(version);
  std::stringstream ss(version_str);

  int64_t major = 0;
  int64_t minor = 0;
  int64_t patch = 0;

  if (!(ss >> major) || ss.get() != '.') {
    return false;
  }

  if (!(ss >> minor) || ss.get() != '.') {
    return false;
  }

  if (!(ss >> patch)) {
    return false;
  }

  if (!ss.eof()) {
    char remaining = '\0';
    if (ss >> remaining) {
      return false;
    }
  }

  if (major < 0 || minor < 0 || patch < 0) {
    return false;
  }

  out_ver->major = static_cast<int>(major);
  out_ver->minor = static_cast<int>(minor);
  out_ver->patch = static_cast<int>(patch);

  return true;
}

bool compare_to_minor_version(const struct semver * v1, const struct semver * v2)
{
  if (v1 == nullptr || v2 == nullptr) {
    return false;
  }

  return (v1->major == v2->major && v1->minor == v2->minor);
}

bool compare_to_patch_version(const struct semver * v1, const struct semver * v2)
{
  if (v1 == nullptr || v2 == nullptr) {
    return false;
  }

  return (v1->major == v2->major && v1->minor == v2->minor && v1->patch == v2->patch);
}

bool is_version_consistent(
  const unsigned char * heaphook_version_ptr, const size_t heaphook_version_str_len,
  struct ioctl_get_version_args kmod_version)
{
  std::array<char, VERSION_BUFFER_LEN> heaphook_version_arr{};
  struct semver lib_ver
  {
  };
  struct semver heaphook_ver
  {
  };
  struct semver kmod_ver
  {
  };

  size_t copy_len = heaphook_version_str_len < (VERSION_BUFFER_LEN - 1) ? heaphook_version_str_len
                                                                        : (VERSION_BUFFER_LEN - 1);
  std::memcpy(heaphook_version_arr.data(), heaphook_version_ptr, copy_len);
  heaphook_version_arr[copy_len] = '\0';

  bool parse_lib_result = parse_semver(hermoslib::VERSION, &lib_ver);
  bool parse_heaphook_result = parse_semver(heaphook_version_arr.data(), &heaphook_ver);
  bool parse_kmod_result =
    parse_semver(static_cast<const char *>(&kmod_version.ret_version[0]), &kmod_ver);

  if (!parse_lib_result || !parse_heaphook_result || !parse_kmod_result) {
    RCLCPP_ERROR(logger, "Failed to parse one or more version strings");
    return false;
  }

  if (!compare_to_patch_version(&lib_ver, &heaphook_ver)) {
    RCLCPP_ERROR(
      logger,
      "Hermos Heaphook and Hermoslib versions must match exactly: Major, Minor, and Patch "
      "versions must all be identical. (hermos-heaphook(%d.%d.%d), hermos(%d.%d.%d))",
      heaphook_ver.major, heaphook_ver.minor, heaphook_ver.patch, lib_ver.major, lib_ver.minor,
      lib_ver.patch);
    return false;
  }

  // Registration checks the notification ABI separately because multiple 2.3.5 builds exist.
  if (!compare_to_minor_version(&lib_ver, &kmod_ver)) {
    RCLCPP_ERROR(
      logger,
      "Hermos Kernel Module and Hermoslib must be compatible: Major and Minor versions must "
      "match. (hermos-kmod(%d.%d.%d), hermos(%d.%d.%d))",
      kmod_ver.major, kmod_ver.minor, kmod_ver.patch, lib_ver.major, lib_ver.minor, lib_ver.patch);
    return false;
  }

  return true;
}

// Opt-out for deployments that manage the discovery agent themselves. getenv()
// does not allocate, so this is safe before hermos's allocator is ready.
bool discovery_agent_auto_fork_disabled()
{
  const char * v = getenv("HERMOS_NO_DISCOVERY_AGENT");
  return v != nullptr &&
         (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0);
}

template <typename Func>
pid_t spawn_daemon_process(Func && func)
{
  auto fail = [](const char * err_fmt) {
    RCLCPP_ERROR(logger, err_fmt, strerror(errno));
    close(hermos_fd);
    exit(EXIT_FAILURE);
  };

  pid_t pid = fork();
  if (pid < 0) {
    fail("fork failed: %s");
  }
  if (pid == 0) {
    hermos::is_bridge_process = true;
    unsetenv("LD_PRELOAD");

    // Redirect stdio to /dev/null when stdout or stderr is an inherited pipe or socket. In that
    // case, a process may be reading from the pipe and waiting on it to close, which can cause
    // the process to hang because the daemon never closes it. Redirecting to /dev/null works around
    // this issue.
    struct stat st_out = {};
    struct stat st_err = {};
    if (fstat(STDOUT_FILENO, &st_out) < 0) {
      fail("fstat for stdout failed: %s");
    }
    if (fstat(STDERR_FILENO, &st_err) < 0) {
      fail("fstat for stderr failed: %s");
    }

    if (
      S_ISFIFO(st_out.st_mode) || S_ISFIFO(st_err.st_mode) || S_ISSOCK(st_out.st_mode) ||
      S_ISSOCK(st_err.st_mode)) {
      int devnull = open("/dev/null", O_RDWR);
      if (devnull < 0) {
        fail("Failed to open /dev/null: %s");
      }

      if (dup2(devnull, STDIN_FILENO) < 0) {
        fail("dup2 for stdin failed: %s");
      }
      if (dup2(devnull, STDOUT_FILENO) < 0) {
        fail("dup2 for stdout failed: %s");
      }
      if (dup2(devnull, STDERR_FILENO) < 0) {
        fail("dup2 for stderr failed: %s");
      }
      close(devnull);
    }

    if (setsid() == -1) {
      fail("setsid failed: %s");
    }

    func();
    exit(0);
  }

  return pid;
}

// NOTE: Avoid heap allocation inside initialize_hermos. TLSF is not initialized yet.
struct initialize_hermos_result initialize_hermos(
  const unsigned char * heaphook_version_ptr, const size_t heaphook_version_str_len)
{
  if (hermos_fd >= 0) {
    RCLCPP_ERROR(logger, "Hermos is already open");
    exit(EXIT_FAILURE);
  }

  const char * device_path = get_hermos_device_path();
  hermos_fd = open(device_path, O_RDWR);
  if (hermos_fd < 0) {
    if (errno == ENOENT) {
      RCLCPP_ERROR(logger, "%s (device path: %s)", HERMOS_DEVICE_NOT_FOUND_MSG, device_path);
    } else {
      RCLCPP_ERROR(logger, "Failed to open %s: %s", device_path, strerror(errno));
    }
    exit(EXIT_FAILURE);
  }
  reserve_heap_range(device_path);

  struct ioctl_get_version_args get_version_args = {};
  if (ioctl(hermos_fd, HERMOS_GET_VERSION_CMD, &get_version_args) < 0) {
    RCLCPP_ERROR(logger, "HERMOS_GET_VERSION_CMD failed: %s", strerror(errno));
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }

  if (!is_version_consistent(heaphook_version_ptr, heaphook_version_str_len, get_version_args)) {
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }

  union ioctl_add_process_args add_process_args = {};
  add_process_args.domain_id = get_ros_domain_id();
  if (ioctl(hermos_fd, HERMOS_ADD_PROCESS_CMD, &add_process_args) < 0) {
    RCLCPP_ERROR(logger, "HERMOS_ADD_PROCESS_CMD failed: %s", strerror(errno));
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }

  bool should_spawn_bridge = false;
  auto bridge_mode = get_bridge_mode();

  // Create a shm_unlink daemon process if it doesn't exist in its ipc namespace.
  if (!add_process_args.ret_unlink_daemon_exist) {
    spawn_daemon_process([]() { poll_for_unlink(); });
  }
  if (bridge_mode == BridgeMode::On && !add_process_args.ret_performance_bridge_daemon_exist) {
    should_spawn_bridge = true;
  }

  if (should_spawn_bridge) {
    spawn_daemon_process([]() { poll_for_bridge_manager(); });
  }

  // The forked agent inherits this process's IPC namespace and ROS_DOMAIN_ID, and
  // self-exits when the scope empties. A missing or unstartable agent is not fatal
  // (the data plane does not depend on the observer); a fork() failure still is, as
  // for the other daemons spawned here -- it means system-wide resource exhaustion.
  if (!add_process_args.ret_discovery_agent_exist && !discovery_agent_auto_fork_disabled()) {
    spawn_daemon_process([]() { exec_discovery_agent(); });
  }

  void * mempool_ptr =
    map_writable_area(getpid(), add_process_args.ret_addr, add_process_args.ret_shm_size);
  if (mempool_ptr == nullptr) {
    close(hermos_fd);
    exit(EXIT_FAILURE);
  }

  struct initialize_hermos_result result = {};
  result.mempool_ptr = mempool_ptr;
  result.mempool_size = add_process_args.ret_shm_size;
  return result;
}

static void shutdown_hermos()
{
  std::lock_guard<std::mutex> lock(shm_fds_mtx);
  for (int fd : shm_fds) {
    if (close(fd) == -1) {
      perror("[ERROR] [Hermos] close shm_fd failed");
    }
  }
}

class Cleanup
{
public:
  Cleanup(const Cleanup &) = delete;
  Cleanup & operator=(const Cleanup &) = delete;
  Cleanup(Cleanup &&) = delete;
  Cleanup & operator=(Cleanup &&) = delete;

  Cleanup() = default;
  ~Cleanup() { shutdown_hermos(); }
};

static Cleanup cleanup;

}  // namespace hermos
