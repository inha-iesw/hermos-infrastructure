#pragma once

#include "hermos/hermos_public_api.hpp"
#include "hermos/node/hermos_arguments.hpp"

#include <mutex>
#include <string>

namespace hermos
{

class Context
{
  struct CommandLineParams
  {
    std::string node_name;
  };

public:
  CommandLineParams command_line_params;

  void init(int argc, char const * const * argv);
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

/// @brief Initialize Hermos. Must be called once before creating any hermos::Node.
/// This is the counterpart of rclcpp::init() for hermos::Node.
/// @param argc Number of command-line arguments.
/// @param argv Command-line argument array.
HERMOS_PUBLIC
void init(int argc, char const * const * argv);

/// @brief Shut down Hermos. Should be called before process exit in hermos::Node processes.
/// This is the counterpart of rclcpp::shutdown() for hermos::Node.
HERMOS_PUBLIC
void shutdown();

/// @brief Check whether Hermos context is valid (initialized and not shutdown).
/// This is the counterpart of rclcpp::ok() for hermos::Node.
HERMOS_PUBLIC
bool ok();

}  // namespace hermos
