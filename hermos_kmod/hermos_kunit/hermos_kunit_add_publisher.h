/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
#pragma once
#include <kunit/test.h>

#define TEST_CASES_ADD_PUBLISHER                                                        \
  KUNIT_CASE(test_case_add_publisher_normal), KUNIT_CASE(test_case_add_publisher_many), \
    KUNIT_CASE(test_case_add_publisher_too_many), \
    KUNIT_CASE(test_case_add_publisher_requires_registration), \
    KUNIT_CASE(test_case_add_publisher_rejects_exited_process), \
    KUNIT_CASE(test_case_add_publisher_rejects_namespace_mismatch)

void test_case_add_publisher_normal(struct kunit * test);
void test_case_add_publisher_many(struct kunit * test);
void test_case_add_publisher_too_many(struct kunit * test);

void test_case_add_publisher_requires_registration(struct kunit * test);
void test_case_add_publisher_rejects_exited_process(struct kunit * test);
void test_case_add_publisher_rejects_namespace_mismatch(struct kunit * test);

