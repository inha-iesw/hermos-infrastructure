## How to integrate Hermos to Autoware

Basically, take a look at the first Autoware integration Pull Request <https://github.com/tier4/autoware.universe_tmp-agnocast/pull/2>

There are two steps for Hermos to work with Autoware:

### Step 1: Add Hermos dependencies

For CMakeLists.txt (`target_library` should be replaced with the corresponding target):

```c++
find_package(hermoslib REQUIRED)
...
ament_target_dependencies(target_library hermoslib)
target_include_directories(target_library PRIVATE
  ${hermoslib_INCLUDE_DIRS}
)
```

For launch.xml:

```xml
<env name="LD_PRELOAD" value="libhermos_heaphook.so"/>
```

For packages.xml:

```xml
<depend>hermoslib</depend>
```

### Step 2: Replace ROS 2 APIs for Hermos APIs

The declarations and initializations should be replaced like the following:

```c++
// rclcpp::Publisher<MessageType>::SharedPtr message_pub_;
// message_pub_ = this->create_publisher<MessageType>("/topic_name", rclcpp::QoS{x});

std::shared_ptr<hermos::Publisher<MessageType>> message_pub_;
message_pub_ = hermos::create_publisher<MessageType>("/topic_name", rclcpp::QoS{x});
```

```c++
// rclcpp::Subscription<MessageType>::SharedPtr> message_sub_;
// message_sub_ = node_.create_subscription<MessageType>("/topic_name", rclcpp::QoS{x}, callback);

std::shared_ptr<hermos::Subscription<MessageType>> message_sub_;
message_sub_ = hermos::create_subscription<MessageType>("/topic_name", rclcpp::QoS{x}, callback);
```

### Other tips

- Until the subscription callback thread is integrated into ROS 2 executor, all callback functions should be guarded with mutex lock.
- Although Hermos already has `get_subscription_count()` API, it is not still complete. There is an issue ticket <https://github.com/autowarefoundation/agnocast/issues/181>.
- Hermos does not support `publish_if_subscribed()` API yet. There is an issue ticket <https://github.com/autowarefoundation/agnocast/issues/164>.
