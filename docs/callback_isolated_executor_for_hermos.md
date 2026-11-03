# Callback Isolated Executor for Hermos

[`hermos::CallbackIsolatedHermosExecutor`](../src/hermoslib/include/hermos/hermos_callback_isolated_executor.hpp) and [`hermos_component_container_cie`](../src/hermoslib/src/hermos_component_container_cie.cpp) are the Hermos versions of the Callback Isolated Executor, which can handle both ROS 2 callbacks and Hermos callbacks. For basic usage, please refer to the [callback_isolated_executor README](https://github.com/autowarefoundation/callback_isolated_executor/blob/main/README.md).

## Differences from the Original

### Package, Class, and Executable Names

|                      | Original                                | Hermos Version                                                  |
| -------------------- | --------------------------------------- | ----------------------------------------------------------------- |
| Package              | `callback_isolated_executor`            | `hermos_components` (recommended) or `hermoslib` (deprecated) |
| Executor Class       | `CallbackIsolatedExecutor`              | `hermos::CallbackIsolatedHermosExecutor`                      |
| Container Executable | `component_container_callback_isolated` | `hermos_component_container_cie`                                |

### Features

#### `hermos_components_register_node` Macro

Instead of manually configuring component containers and launch files, you can use the `hermos_components_register_node()` CMake macro (provided by the [`hermos_components`](../src/hermos_components) package) as a drop-in replacement for `rclcpp_components_register_node()`. This macro generates a standalone executable that uses the specified Hermos executor.

```cmake
find_package(hermos_components REQUIRED)

add_library(my_component SHARED src/my_node.cpp)
ament_target_dependencies(my_component rclcpp rclcpp_components hermoslib ...)

hermos_components_register_node(
  my_component
  PLUGIN "MyNode"
  EXECUTABLE my_node
  EXECUTOR CallbackIsolatedHermosExecutor
)
```

#### Multiple ROS domain support

`hermos_cie_thread_configurator` can handle callback groups from multiple ROS domains. Use ROS parameters to specify domain IDs:

```bash
# Option 1: Run prerun_node directly with ROS parameters
ros2 run hermos_cie_thread_configurator prerun_node --ros-args -p domains:=[0,1]

# Option 2: Use the launch file
ros2 launch hermos_cie_thread_configurator thread_configurator.launch.xml prerun:=true domains:=[0,1]
```

#### RT Throttling

The `rt_throttling` feature configures the kernel's real-time scheduling bandwidth parameters (`sched_rt_period_us` and `sched_rt_runtime_us`). At startup, `hermos_cie_thread_configurator` validates that the current kernel values match the configuration and reports an error if they differ.

Since `/proc/sys/kernel/sched_rt_period_us` and `/proc/sys/kernel/sched_rt_runtime_us` can only be written by root (uid 0) — Linux capabilities such as `CAP_SYS_ADMIN` cannot bypass the `/proc/sys/` permission check — these values must be pre-configured via `/etc/sysctl.d/`:

```bash
# /etc/sysctl.d/99-rt-throttling.conf
kernel.sched_rt_period_us = 1000000
kernel.sched_rt_runtime_us = 950000
```

After creating the file, apply it immediately with:

```bash
sudo sysctl --system
```

The values will also be applied automatically on subsequent boots.
