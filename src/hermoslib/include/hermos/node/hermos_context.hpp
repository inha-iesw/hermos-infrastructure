#pragma once

#include "hermos/hermos_public_api.hpp"
#include "hermos/node/hermos_arguments.hpp"

#include <mutex>
#include <string>

namespace hermos
{

/** @brief Select ownership of process-global services during initialization. */
struct InitOptions
{
  /**
   * @brief Configure rcl logging and finalize it on explicit Hermos shutdown.
   *
   * Set false when an embedding runtime owns logging. That runtime must keep
   * logging available until Hermos shutdown completes.
   */
  bool configure_logging = true;
};

class Context
{
  struct CommandLineParams
  {
    std::string node_name;
  };

public:
  CommandLineParams command_line_params;

  void init(int argc, char const * const * argv);
  /**
   * @brief Initialize context state using the selected logging policy.
   *
   * An initialized context ignores subsequent options. Direct Context users
   * manage logging cleanup themselves; Context::shutdown() only invalidates state.
   * Use hermos::init()/shutdown() to manage the process-global lifetime.
   *
   * @param argc Number of command-line arguments.
   * @param argv Borrowed argument array; may be null when argc is zero.
   * @param options Logging policy for this initialization.
   * @return Nothing.
   * @throws std::runtime_error If ROS arguments cannot be parsed.
   */
  void init(int argc, char const * const * argv, const InitOptions & options);
  void shutdown();
  bool is_initialized() const { return initialized_; }

  const rcl_arguments_t * get_parsed_arguments() const
  {
    return parsed_arguments_.is_valid() ? parsed_arguments_.get() : nullptr;
  }

private:
  bool initialized_ = false;
  ParsedArguments parsed_arguments_;
};

extern Context g_context;
extern std::mutex g_context_mtx;

/**
 * @brief Initialize Hermos with standalone logging ownership.
 *
 * Equivalent to init(argc, argv, InitOptions{}).
 *
 * @param argc Number of command-line arguments.
 * @param argv Borrowed argument array; may be null when argc is zero.
 * @return Nothing.
 */
HERMOS_PUBLIC
void init(int argc, char const * const * argv);

/**
 * @brief Initialize Hermos with explicit process-global logging ownership.
 *
 * Must precede creation of hermos::Node. An already initialized context keeps
 * its original policy. Signal handler installation and chaining are unchanged.
 *
 * @param argc Number of command-line arguments.
 * @param argv Borrowed argument array; may be null when argc is zero.
 * @param options Set configure_logging=false to preserve an embedding runtime's logging.
 * @return Nothing.
 * @pre Serialize init/shutdown calls and explicitly shut down each lifetime before restarting.
 * @throws std::runtime_error If ROS arguments cannot be parsed.
 */
HERMOS_PUBLIC
void init(int argc, char const * const * argv, const InitOptions & options);

/**
 * @brief Stop Hermos and finalize only logging owned by its initialization.
 *
 * Explicit shutdown is required even after a signal invalidates the context.
 * Calls without an owned logging lifetime leave external logging untouched.
 *
 * @return Nothing.
 */
HERMOS_PUBLIC
void shutdown();

/// @brief Check whether Hermos context is valid (initialized and not shutdown).
/// This is the counterpart of rclcpp::ok() for hermos::Node.
HERMOS_PUBLIC
bool ok();

}  // namespace hermos
