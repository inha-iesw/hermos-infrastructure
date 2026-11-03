cmake_minimum_required(VERSION 3.14)
project(hermos_sample_application)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_components REQUIRED)

find_package(hermoslib REQUIRED)
find_package(hermos_sample_interfaces REQUIRED)
find_package(hermos_components REQUIRED)

add_executable(talker src/minimal_publisher.cpp)
ament_target_dependencies(talker rclcpp hermoslib hermos_sample_interfaces)
target_include_directories(talker PRIVATE
  ${hermoslib_INCLUDE_DIRS}
)

add_executable(generic_talker src/minimal_generic_publisher.cpp)
ament_target_dependencies(generic_talker rclcpp hermoslib hermos_sample_interfaces)
target_include_directories(generic_talker PRIVATE
  ${hermoslib_INCLUDE_DIRS}
)

add_executable(cie_tutorial_node src/cie_tutorial_node.cpp)
ament_target_dependencies(cie_tutorial_node rclcpp hermoslib)

add_executable(cie_talker src/cie_publisher.cpp)
ament_target_dependencies(cie_talker rclcpp hermoslib hermos_sample_interfaces)
target_include_directories(cie_talker PRIVATE
  ${hermoslib_INCLUDE_DIRS}
)

add_library(hermos_cie_listener_component SHARED src/cie_subscriber.cpp)
ament_target_dependencies(hermos_cie_listener_component rclcpp rclcpp_components hermoslib hermos_sample_interfaces)
target_include_directories(hermos_cie_listener_component PRIVATE ${hermoslib_INCLUDE_DIRS})

hermos_components_register_node(
  hermos_cie_listener_component
  PLUGIN "CieSubscriber"
  EXECUTABLE cie_listener
)

add_library(hermos_no_rclcpp_listener_component SHARED src/no_rclcpp_subscriber.cpp)
ament_target_dependencies(hermos_no_rclcpp_listener_component rclcpp rclcpp_components hermoslib hermos_sample_interfaces)
target_include_directories(hermos_no_rclcpp_listener_component PRIVATE ${hermoslib_INCLUDE_DIRS})

hermos_components_register_node(
  hermos_no_rclcpp_listener_component
  PLUGIN "NoRclcppSubscriber"
  EXECUTABLE no_rclcpp_listener
  EXECUTOR HermosOnlySingleThreadedExecutor
)

add_executable(no_rclcpp_pubsub src/no_rclcpp_pubsub.cpp)
ament_target_dependencies(no_rclcpp_pubsub hermoslib hermos_sample_interfaces)
target_include_directories(no_rclcpp_pubsub PRIVATE
  ${hermoslib_INCLUDE_DIRS}
)

add_executable(no_rclcpp_talker src/no_rclcpp_publisher.cpp)
ament_target_dependencies(no_rclcpp_talker hermoslib hermos_sample_interfaces)
target_include_directories(no_rclcpp_talker PRIVATE
  ${hermoslib_INCLUDE_DIRS}
)

add_executable(no_rclcpp_take_listener src/no_rclcpp_take_subscriber.cpp)
ament_target_dependencies(no_rclcpp_take_listener hermoslib hermos_sample_interfaces)
target_include_directories(no_rclcpp_take_listener PRIVATE
  ${hermoslib_INCLUDE_DIRS}
)

add_executable(client src/minimal_client.cpp)
ament_target_dependencies(client rclcpp hermoslib hermos_sample_interfaces)
target_include_directories(client PRIVATE
  ${hermoslib_INCLUDE_DIRS}
)

add_executable(server src/minimal_server.cpp)
ament_target_dependencies(server rclcpp hermoslib hermos_sample_interfaces)
target_include_directories(server PRIVATE
  ${hermoslib_INCLUDE_DIRS}
)

add_executable(no_rclcpp_server src/no_rclcpp_server.cpp)
ament_target_dependencies(no_rclcpp_server hermoslib hermos_sample_interfaces)
target_include_directories(no_rclcpp_server PRIVATE
  ${hermoslib_INCLUDE_DIRS}
)

add_executable(no_rclcpp_client src/no_rclcpp_client.cpp)
ament_target_dependencies(no_rclcpp_client hermoslib hermos_sample_interfaces)
target_include_directories(no_rclcpp_client PRIVATE
  ${hermoslib_INCLUDE_DIRS}
)

add_executable(sim_time_timer src/sim_time_timer.cpp)
ament_target_dependencies(sim_time_timer rclcpp hermoslib)
target_include_directories(sim_time_timer PRIVATE
  ${hermoslib_INCLUDE_DIRS}
)

add_library(hermos_listener_component SHARED src/minimal_subscriber.cpp)
ament_target_dependencies(hermos_listener_component rclcpp rclcpp_components hermoslib hermos_sample_interfaces)
target_include_directories(hermos_listener_component PRIVATE ${hermoslib_INCLUDE_DIRS})

hermos_components_register_node(
  hermos_listener_component
  PLUGIN "MinimalSubscriber"
  EXECUTABLE listener
)

ament_export_targets(export_hermos_listener_component)
install(TARGETS hermos_listener_component
        EXPORT export_hermos_listener_component
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
)

add_library(hermos_generic_listener_component SHARED src/minimal_generic_subscriber.cpp)
ament_target_dependencies(hermos_generic_listener_component rclcpp rclcpp_components hermoslib hermos_sample_interfaces)
target_include_directories(hermos_generic_listener_component PRIVATE ${hermoslib_INCLUDE_DIRS})

hermos_components_register_node(
  hermos_generic_listener_component
  PLUGIN "MinimalGenericSubscriber"
  EXECUTABLE generic_listener
)

ament_export_targets(export_hermos_generic_listener_component)
install(TARGETS hermos_generic_listener_component
        EXPORT export_hermos_generic_listener_component
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
)

ament_export_targets(export_hermos_cie_listener_component)
install(TARGETS hermos_cie_listener_component
        EXPORT export_hermos_cie_listener_component
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
)

install(TARGETS talker
  DESTINATION lib/${PROJECT_NAME})

install(TARGETS generic_talker
  DESTINATION lib/${PROJECT_NAME})

install(TARGETS cie_talker
  DESTINATION lib/${PROJECT_NAME})

install(TARGETS cie_tutorial_node
  DESTINATION lib/${PROJECT_NAME})

ament_export_targets(export_hermos_no_rclcpp_listener_component)
install(TARGETS hermos_no_rclcpp_listener_component
        EXPORT export_hermos_no_rclcpp_listener_component
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
)

install(TARGETS no_rclcpp_pubsub
  DESTINATION lib/${PROJECT_NAME})

install(TARGETS no_rclcpp_talker
  DESTINATION lib/${PROJECT_NAME})

install(TARGETS no_rclcpp_take_listener
  DESTINATION lib/${PROJECT_NAME})

install(TARGETS client
  DESTINATION lib/${PROJECT_NAME})

install(TARGETS server
  DESTINATION lib/${PROJECT_NAME})

install(TARGETS no_rclcpp_server
  DESTINATION lib/${PROJECT_NAME})

install(TARGETS no_rclcpp_client
  DESTINATION lib/${PROJECT_NAME})

install(TARGETS sim_time_timer
  DESTINATION lib/${PROJECT_NAME})

install(DIRECTORY launch
  DESTINATION share/${PROJECT_NAME}/
)

ament_package()
