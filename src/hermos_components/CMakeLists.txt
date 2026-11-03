cmake_minimum_required(VERSION 3.14)
project(hermos_components)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_components REQUIRED)
find_package(class_loader REQUIRED)
find_package(hermoslib REQUIRED)
find_package(hermos_cie_thread_configurator REQUIRED)
find_package(hermos_cie_config_msgs REQUIRED)
find_package(glog REQUIRED)

add_executable(hermos_component_container src/hermos_component_container.cpp)
target_link_libraries(hermos_component_container glog::glog)
ament_target_dependencies(hermos_component_container rclcpp rclcpp_components hermoslib)

add_executable(hermos_component_container_mt src/hermos_component_container_mt.cpp)
target_link_libraries(hermos_component_container_mt glog::glog)
ament_target_dependencies(hermos_component_container_mt rclcpp rclcpp_components hermoslib)

add_executable(hermos_component_container_cie src/hermos_component_container_cie.cpp)
target_link_libraries(hermos_component_container_cie glog::glog)
ament_target_dependencies(hermos_component_container_cie rclcpp rclcpp_components hermoslib hermos_cie_thread_configurator hermos_cie_config_msgs)

install(
  TARGETS hermos_component_container hermos_component_container_mt hermos_component_container_cie
  DESTINATION lib/${PROJECT_NAME}
)

install(
  DIRECTORY cmake/
  DESTINATION share/${PROJECT_NAME}/cmake
)

install(
  FILES src/node_main.cpp.in src/node_main_hermos_only.cpp.in
  DESTINATION share/${PROJECT_NAME}/src
)

ament_export_dependencies(
  rclcpp
  rclcpp_components
  class_loader
  hermoslib
)

ament_package(
  CONFIG_EXTRAS
    "hermos_components-extras.cmake.in"
)
