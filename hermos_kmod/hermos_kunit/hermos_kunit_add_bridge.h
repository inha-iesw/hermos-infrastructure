/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
#pragma once
#include <kunit/test.h>

#define TEST_CASES_ADD_BRIDGE                                                             \
  KUNIT_CASE(test_case_add_bridge_normal), KUNIT_CASE(test_case_add_bridge_update_flags), \
    KUNIT_CASE(test_case_add_bridge_already_exists_diff_pid), \
    KUNIT_CASE(test_case_add_bridge_requires_registration), \
    KUNIT_CASE(test_case_add_bridge_rejects_namespace_mismatch), \
    KUNIT_CASE(test_case_add_bridge_exit_cleanup)

void test_case_add_bridge_normal(struct kunit * test);
void test_case_add_bridge_update_flags(struct kunit * test);
void test_case_add_bridge_already_exists_diff_pid(struct kunit * test);
void test_case_add_bridge_requires_registration(struct kunit * test);
void test_case_add_bridge_rejects_namespace_mismatch(struct kunit * test);
void test_case_add_bridge_exit_cleanup(struct kunit * test);
