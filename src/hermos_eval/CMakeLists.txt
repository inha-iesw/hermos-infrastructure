cmake_minimum_required(VERSION 3.14)
project(hermos_eval)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(hermoslib REQUIRED)
find_package(hermos_sample_interfaces REQUIRED)

foreach(target hermos_talker hermos_hold_listener std_talker std_hold_listener)
  add_executable(${target} src/${target}.cpp)
  ament_target_dependencies(${target} rclcpp hermoslib hermos_sample_interfaces)
  target_include_directories(${target} PRIVATE ${hermoslib_INCLUDE_DIRS})
endforeach()

# Size probe needs no hermoslib (pure rclcpp serialization + the message type).
add_executable(msg_size_probe src/msg_size_probe.cpp)
ament_target_dependencies(msg_size_probe rclcpp hermos_sample_interfaces)

install(TARGETS hermos_talker hermos_hold_listener std_talker std_hold_listener msg_size_probe
  DESTINATION lib/${PROJECT_NAME})

ament_package()
