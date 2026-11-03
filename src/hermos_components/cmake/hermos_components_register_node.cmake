# Register an rclcpp component with Hermos executor support
# and create a standalone executable.
#
# usage: hermos_components_register_node(
#        <target> PLUGIN <component> EXECUTABLE <node>
#        [EXECUTOR <executor_type>]
#        [RESOURCE_INDEX <resource_index>])
#
# :param target: the shared library target
# :type target: string
# :param PLUGIN: the plugin name (fully qualified class name)
# :type PLUGIN: string
# :param EXECUTABLE: the node's executable name
# :type EXECUTABLE: string
# :param EXECUTOR: the Hermos executor type (default: SingleThreadedHermosExecutor)
# :type EXECUTOR: string
#   For rclcpp::Node (uses rclcpp::init):
#     - SingleThreadedHermosExecutor
#     - MultiThreadedHermosExecutor
#     - CallbackIsolatedHermosExecutor
#   For hermos::Node (uses hermos::init):
#     - HermosOnlySingleThreadedExecutor
#     - HermosOnlyMultiThreadedExecutor
#     - HermosOnlyCallbackIsolatedExecutor
# :param RESOURCE_INDEX: the ament resource index to register the components (default: rclcpp_components)
# :type RESOURCE_INDEX: string
#
macro(hermos_components_register_node target)
  cmake_parse_arguments(ARGS "SKIP_LIBRARY_DEPENDENCY" "PLUGIN;EXECUTABLE;EXECUTOR;RESOURCE_INDEX" "" ${ARGN})

  if(ARGS_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "hermos_components_register_node() called with unused "
      "arguments: ${ARGS_UNPARSED_ARGUMENTS}")
  endif()

  if("${ARGS_PLUGIN}" STREQUAL "")
    message(FATAL_ERROR "hermos_components_register_node macro requires a PLUGIN argument for target ${target}")
  endif()

  if("${ARGS_EXECUTABLE}" STREQUAL "")
    message(FATAL_ERROR "hermos_components_register_node macro requires an EXECUTABLE argument for target ${target}")
  endif()

  # Default to rclcpp_components resource index for compatibility with existing containers
  set(resource_index "rclcpp_components")
  if(NOT "${ARGS_RESOURCE_INDEX}" STREQUAL "")
    set(resource_index ${ARGS_RESOURCE_INDEX})
    message(STATUS "hermos_components: Setting component resource index to ${resource_index}")
  endif()

  # Default to SingleThreadedHermosExecutor
  set(executor "SingleThreadedHermosExecutor")
  set(_use_hermos_only_template FALSE)
  if(NOT "${ARGS_EXECUTOR}" STREQUAL "")
    # Validate executor type
    set(_rclcpp_executors "SingleThreadedHermosExecutor;MultiThreadedHermosExecutor;CallbackIsolatedHermosExecutor")
    set(_hermos_only_executors "HermosOnlySingleThreadedExecutor;HermosOnlyMultiThreadedExecutor;HermosOnlyCallbackIsolatedExecutor")
    set(_valid_executors "${_rclcpp_executors};${_hermos_only_executors}")
    if(NOT "${ARGS_EXECUTOR}" IN_LIST _valid_executors)
      message(FATAL_ERROR "hermos_components_register_node: Invalid EXECUTOR '${ARGS_EXECUTOR}'. "
        "Must be one of: ${_valid_executors}")
    endif()
    set(executor ${ARGS_EXECUTOR})
    # Check if using hermos-only executor (for hermos::Node)
    if("${ARGS_EXECUTOR}" IN_LIST _hermos_only_executors)
      set(_use_hermos_only_template TRUE)
      message(STATUS "hermos_components: Using hermos::Node executor ${executor}")
    else()
      message(STATUS "hermos_components: Using rclcpp::Node executor ${executor}")
    endif()
  endif()

  set(component ${ARGS_PLUGIN})
  set(node ${ARGS_EXECUTABLE})

  set(library_name "$<TARGET_FILE_NAME:${target}>")

  # Register with rclcpp_components for component container support (resource index only).
  # Unlike rclcpp_components_register_node (singular), the plural form only populates
  # the ament resource index without generating a standalone executable.
  # This avoids duplicate file(GENERATE) calls on the same ament index file when both
  # hermos_components_register_node and rclcpp_components_register_node are used
  # in the same package.
  rclcpp_components_register_nodes(${target} ${component}
    RESOURCE_INDEX ${resource_index})

  # Select template based on executor type
  if(_use_hermos_only_template)
    set(_node_template ${hermos_components_NODE_TEMPLATE_HERMOS_ONLY})
  else()
    set(_node_template ${hermos_components_NODE_TEMPLATE})
  endif()

  # Generate node main from template
  configure_file(${_node_template}
    ${PROJECT_BINARY_DIR}/hermos_components/node_main_configured_${node}.cpp.in)
  file(GENERATE OUTPUT ${PROJECT_BINARY_DIR}/hermos_components/node_main_${node}.cpp
    INPUT ${PROJECT_BINARY_DIR}/hermos_components/node_main_configured_${node}.cpp.in)

  if(NOT TARGET glog::glog)
    find_package(glog REQUIRED)
  endif()

  # Create executable
  add_executable(${node} ${PROJECT_BINARY_DIR}/hermos_components/node_main_${node}.cpp)
  if(NOT ARGS_SKIP_LIBRARY_DEPENDENCY)
    add_dependencies(${node} ${target})
  endif()
  target_link_libraries(${node} glog::glog)
  ament_target_dependencies(${node}
    "rclcpp"
    "class_loader"
    "rclcpp_components"
    "hermoslib")

  # Install executable
  install(TARGETS
    ${node}
    DESTINATION lib/${PROJECT_NAME})
endmacro()
