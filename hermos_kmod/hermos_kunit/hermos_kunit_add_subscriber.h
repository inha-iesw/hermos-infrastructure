/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
#pragma once
#include <kunit/test.h>

#define TEST_CASES_ADD_SUBSCRIBER                              \
  KUNIT_CASE(test_case_add_subscriber_normal),                 \
    KUNIT_CASE(test_case_add_subscriber_too_many_subscribers), \
    KUNIT_CASE(test_case_add_subscriber_domain_isolation),     \
    KUNIT_CASE(test_case_add_subscriber_domain_mq_tokens_differ), \
    KUNIT_CASE(test_case_add_subscriber_same_domain_shared), \
    KUNIT_CASE(test_case_add_subscriber_requires_registration), \
    KUNIT_CASE(test_case_add_subscriber_rejects_exited_process), \
    KUNIT_CASE(test_case_add_subscriber_rejects_namespace_mismatch)

void test_case_add_subscriber_normal(struct kunit * test);
void test_case_add_subscriber_too_many_subscribers(struct kunit * test);
void test_case_add_subscriber_domain_isolation(struct kunit * test);
void test_case_add_subscriber_domain_mq_tokens_differ(struct kunit * test);
void test_case_add_subscriber_same_domain_shared(struct kunit * test);

void test_case_add_subscriber_requires_registration(struct kunit * test);
void test_case_add_subscriber_rejects_exited_process(struct kunit * test);
void test_case_add_subscriber_rejects_namespace_mismatch(struct kunit * test);

