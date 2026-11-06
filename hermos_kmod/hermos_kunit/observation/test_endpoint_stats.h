/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
#pragma once
#include <kunit/test.h>

#define TEST_CASES_ENDPOINT_STATS \
  KUNIT_CASE(test_endpoint_stats_history_and_references), \
  KUNIT_CASE(test_endpoint_stats_domain_and_bridge_identity), \
  KUNIT_CASE(test_endpoint_stats_excludes_exited_endpoints), \
  KUNIT_CASE(test_endpoint_stats_removed_publisher_keeps_held_reference), \
  KUNIT_CASE(test_endpoint_stats_capacity), \
  KUNIT_CASE(test_endpoint_stats_take_same_message_counts_once), \
  KUNIT_CASE(test_endpoint_stats_ignore_local_publications), \
  KUNIT_CASE(test_endpoint_stats_domain_direction_allowed), \
  KUNIT_CASE(test_endpoint_stats_domain_direction_blocked), \
  KUNIT_CASE(test_endpoint_stats_request_validation)

void test_endpoint_stats_history_and_references(struct kunit * test);
void test_endpoint_stats_domain_and_bridge_identity(struct kunit * test);
void test_endpoint_stats_excludes_exited_endpoints(struct kunit * test);
void test_endpoint_stats_removed_publisher_keeps_held_reference(struct kunit * test);
void test_endpoint_stats_capacity(struct kunit * test);
void test_endpoint_stats_take_same_message_counts_once(struct kunit * test);
void test_endpoint_stats_ignore_local_publications(struct kunit * test);
void test_endpoint_stats_domain_direction_allowed(struct kunit * test);
void test_endpoint_stats_domain_direction_blocked(struct kunit * test);
void test_endpoint_stats_request_validation(struct kunit * test);
