#include "hermos/node/hermos_context.hpp"

#include "hermos/hermos_tracepoint_wrapper.h"
#include "hermos_signal_handler.hpp"

#include <rcl/arguments.h>
#include <rcl/error_handling.h>
#include <rcl/logging.h>
#include <rcutils/logging.h>
#include <rcutils/logging_macros.h>

namespace hermos
{

Context g_context;
std::mutex g_context_mtx;

namespace
{
// Protected by g_context_mtx; signal-driven context invalidation does not release logging.
bool g_owns_logging = false;
}  // namespace

void Context::init(int argc, char const * const * argv)
{
  init(argc, argv, InitOptions{});
}

void Context::init(int argc, char const * const * argv, const InitOptions & options)
{
  if (initialized_) {
    return;
  }

  // Copy argv into a safe container to avoid pointer arithmetic
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  parsed_arguments_ = parse_arguments(args);

  // Initialize rcl logging so that RCLCPP_INFO/WARN/etc. are written to
  // ~/.ros/log/ files via rcl_logging_spdlog, matching rclcpp::init() behavior.
  // This also applies --log-level from parsed_arguments_ via
  // rcl_arguments_get_log_levels() internally.
  // rcl_logging_configure_with_output_handler reads --disable-stdout-logs and
  // --disable-external-lib-logs from parsed_arguments_ and registers only the
  // enabled sub-handlers inside rcl_logging_multiple_output_handler, so stdout
  // can be suppressed while file logging via spdlog is preserved.
  if (options.configure_logging) {
    rcl_allocator_t allocator = rcl_get_default_allocator();
    rcl_ret_t ret = rcl_logging_configure_with_output_handler(
      parsed_arguments_.get(), &allocator, rcl_logging_multiple_output_handler);
    if (ret != RCL_RET_OK) {
      RCUTILS_LOG_ERROR_NAMED(
        "hermos", "Failed to configure logging: %s", rcl_get_error_string().str);
      rcl_reset_error();
    }
  }

  initialized_ = true;
  TRACEPOINT(hermos_init, static_cast<const void *>(this));
}

void Context::shutdown()
{
  if (!initialized_) {
    return;
  }
  initialized_ = false;
}

void init(int argc, char const * const * argv)
{
  init(argc, argv, InitOptions{});
}

void init(int argc, char const * const * argv, const InitOptions & options)
{
  {
    std::lock_guard<std::mutex> lock(g_context_mtx);
    if (!g_context.is_initialized()) {
      g_context.init(argc, argv, options);
      g_owns_logging = g_owns_logging || options.configure_logging;
    }
  }
  SignalHandler::install();
}

void shutdown()
{
  bool finalize_logging;
  {
    std::lock_guard<std::mutex> lock(g_context_mtx);
    g_context.shutdown();
    finalize_logging = g_owns_logging;
    g_owns_logging = false;
  }

  SignalHandler::notify_all_executors();
  SignalHandler::uninstall();

  if (finalize_logging) {
    rcl_ret_t ret = rcl_logging_fini();
    if (ret != RCL_RET_OK) {
      RCUTILS_LOG_ERROR_NAMED(
        "hermos", "Failed to finalize logging: %s", rcl_get_error_string().str);
      rcl_reset_error();
    }
  }
}

bool ok()
{
  std::lock_guard<std::mutex> lock(g_context_mtx);
  return g_context.is_initialized();
}

}  // namespace hermos
