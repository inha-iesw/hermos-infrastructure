cmake_minimum_required(VERSION 3.14)
project(hermos_ioctl_wrapper)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)

add_library(hermos_ioctl_wrapper SHARED src/topic_list.cpp src/node_info.cpp src/topic_info.cpp src/version.cpp src/domain_bridge.cpp src/discovery_agent.cpp src/observation/endpoint_stats.cpp)

ament_target_dependencies(hermos_ioctl_wrapper rclcpp)

install(TARGETS hermos_ioctl_wrapper LIBRARY DESTINATION lib)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(test_endpoint_stats test/observation/test_endpoint_stats.cpp)
  target_include_directories(test_endpoint_stats PRIVATE src)
  target_link_libraries(test_endpoint_stats hermos_ioctl_wrapper)
  target_link_options(test_endpoint_stats PRIVATE "-Wl,--export-dynamic-symbol=ioctl")
endif()

ament_package()
