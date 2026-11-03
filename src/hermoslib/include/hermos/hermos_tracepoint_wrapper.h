#pragma once
#include "tracetools/tracetools.h"

// Compatibility: Jazzy uses _DECLARE_TRACEPOINT, Humble uses DECLARE_TRACEPOINT
#ifndef DECLARE_TRACEPOINT
#define DECLARE_TRACEPOINT(...) _DECLARE_TRACEPOINT(__VA_ARGS__)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// clang-format off

DECLARE_TRACEPOINT(
  hermos_init,
  const void * context_handle)

DECLARE_TRACEPOINT(
  hermos_node_init,
  const void * node_handle,
  const char * node_name,
  const char * namespace_arg)

DECLARE_TRACEPOINT(
  hermos_publisher_init,
  const void * publisher_handle,
  const void * node_handle,
  const char * topic_name,
  const size_t queue_depth)

DECLARE_TRACEPOINT(
  hermos_subscription_init,
  const void * subscription_handle,
  const void * node_handle,
  const void * callback,
  const void * callback_group,
  const char * function_symbol,
  const char * topic_name,
  const size_t queue_depth,
  const uint64_t pid_callback_info_id)

DECLARE_TRACEPOINT(
  hermos_timer_init,
  const void * timer_handle,
  const void * node_handle,
  const void * callback,
  const void * callback_group,
  const char * function_symbol,
  int64_t period)

DECLARE_TRACEPOINT(
  hermos_add_callback_group,
  const void * executor_addr,
  const void * node_handle,
  const void * callback_group_addr,
  const char * group_type_name)

DECLARE_TRACEPOINT(
  hermos_publish,
  const void * publisher_handle,
  const int64_t entry_id)

DECLARE_TRACEPOINT(
  hermos_create_callable,
  const void * callable,
  const int64_t entry_id,
  const uint64_t pid_callback_info_id)

DECLARE_TRACEPOINT(
  hermos_create_timer_callable,
  const void * callable,
  const void * timer_handle)

DECLARE_TRACEPOINT(
  hermos_callable_start,
  const void * callable)

DECLARE_TRACEPOINT(
  hermos_callable_end,
  const void * callable)

DECLARE_TRACEPOINT(
  hermos_take,
  const void * subscription_handle,
  const void * message,
  const int64_t entry_id)

DECLARE_TRACEPOINT(
  hermos_construct_executor,
  const void * executor_addr,
  const char * executor_type_name)

// clang-format on

#ifdef __cplusplus
}
#endif
