KUNIT_TEST_OBJS := \
  observation/test_endpoint_stats.o \
  hermos_kunit_add_process.o \
  hermos_kunit_add_subscriber.o \
  hermos_kunit_add_publisher.o \
  hermos_kunit_remove_subscriber.o \
  hermos_kunit_remove_publisher.o \
  hermos_kunit_release_sub_ref.o \
  hermos_kunit_publish_msg.o \
  hermos_kunit_receive_msg.o \
  hermos_kunit_take_msg.o \
  hermos_kunit_get_publisher_num.o \
  hermos_kunit_get_publisher_qos.o \
  hermos_kunit_get_subscriber_num.o \
  hermos_kunit_get_subscriber_qos.o \
  hermos_kunit_add_bridge.o \
  hermos_kunit_remove_bridge.o \
  hermos_kunit_domain_bridge.o \
  hermos_kunit_set_ros2_subscriber_num.o \
  hermos_kunit_set_ros2_publisher_num.o \
  hermos_kunit_do_exit.o \
  hermos_kunit_get_node_subscriber_topics.o \
  hermos_kunit_get_node_publisher_topics.o \
  hermos_kunit_get_version.o \
  hermos_kunit_bridge_shutdown.o \
  hermos_kunit_check_and_request_bridge_shutdown.o \
  hermos_kunit_get_topic_subscriber_info.o \
  hermos_kunit_get_topic_publisher_info.o \
  hermos_kunit_init_memory_allocator.o

hermos_kunit-objs += $(addprefix hermos_kunit/, $(KUNIT_TEST_OBJS))
