// SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause
#include "hermos_kunit_init_memory_allocator.h"

#include "../hermos_memory_allocator.h"

#include <kunit/test.h>
#include <linux/limits.h>

// Helper to save and restore module parameters around each test.
// Tests call init_memory_allocator() with modified globals, then
// cleanup_memory_allocator() to free, and finally restore the originals
// so the suite-wide allocator (re-initialized in hermos_test_exit) is unaffected.

struct saved_params
{
  int mempool_num;
  unsigned long mempool_start_addr;
  int mempool_size_gb;
};

static void save_params(struct saved_params * p)
{
  p->mempool_num = mempool_num;
  p->mempool_start_addr = mempool_start_addr;
  p->mempool_size_gb = mempool_size_gb;
}

static void restore_params(const struct saved_params * p)
{
  mempool_num = p->mempool_num;
  mempool_start_addr = p->mempool_start_addr;
  mempool_size_gb = p->mempool_size_gb;
}

void test_case_init_memory_allocator_non_default(struct kunit * test)
{
  struct saved_params saved;
  save_params(&saved);

  // Use small non-default values
  mempool_num = 2;
  mempool_size_gb = 1;
  mempool_start_addr = 0x80000000000;

  int ret = init_memory_allocator();
  KUNIT_ASSERT_EQ(test, ret, 0);

  // Verify by assigning memory to a process
  struct mempool_entry * entry = assign_memory(9999);
  KUNIT_ASSERT_NOT_NULL(test, entry);
  KUNIT_EXPECT_EQ(test, entry->addr, (uint64_t)0x80000000000);

  free_memory(9999);
  cleanup_memory_allocator();

  // Restore and re-init for subsequent tests
  restore_params(&saved);
  KUNIT_ASSERT_EQ(test, init_memory_allocator(), 0);
}

void test_case_init_memory_allocator_zero_mempool_num(struct kunit * test)
{
  struct saved_params saved;
  save_params(&saved);

  mempool_num = 0;

  int ret = init_memory_allocator();
  KUNIT_EXPECT_EQ(test, ret, -EINVAL);

  restore_params(&saved);
}

void test_case_init_memory_allocator_negative_mempool_num(struct kunit * test)
{
  struct saved_params saved;
  save_params(&saved);

  mempool_num = -1;

  int ret = init_memory_allocator();
  KUNIT_EXPECT_EQ(test, ret, -EINVAL);

  restore_params(&saved);
}

void test_case_init_memory_allocator_zero_mempool_size_gb(struct kunit * test)
{
  struct saved_params saved;
  save_params(&saved);

  mempool_size_gb = 0;

  int ret = init_memory_allocator();
  KUNIT_EXPECT_EQ(test, ret, -EINVAL);

  restore_params(&saved);
}

void test_case_init_memory_allocator_negative_mempool_size_gb(struct kunit * test)
{
  struct saved_params saved;
  save_params(&saved);

  mempool_size_gb = -1;

  int ret = init_memory_allocator();
  KUNIT_EXPECT_EQ(test, ret, -EINVAL);

  restore_params(&saved);
}

void test_case_init_memory_allocator_overflow_mul(struct kunit * test)
{
  struct saved_params saved;
  save_params(&saved);

  // INT_MAX pools * large size_gb should overflow uint64_t multiplication
  mempool_num = INT_MAX;
  mempool_size_gb = INT_MAX;

  int ret = init_memory_allocator();
  KUNIT_EXPECT_EQ(test, ret, -EINVAL);

  restore_params(&saved);
}

void test_case_init_memory_allocator_overflow_add(struct kunit * test)
{
  struct saved_params saved;
  save_params(&saved);

  // Large start address + moderate total size should overflow uint64_t addition
  mempool_start_addr = ULONG_MAX;
  mempool_num = 1;
  mempool_size_gb = 1;

  int ret = init_memory_allocator();
  KUNIT_EXPECT_EQ(test, ret, -EINVAL);

  restore_params(&saved);
}

void test_case_memory_batch_capacity_failure_is_atomic(struct kunit * test)
{
  struct mempool_entry * pools[2] = {assign_memory(1000), assign_memory(1001)};
  KUNIT_ASSERT_NOT_NULL(test, pools[0]);
  KUNIT_ASSERT_NOT_NULL(test, pools[1]);
  bool added[2] = {};
  KUNIT_EXPECT_EQ(test, reference_memory_batch(pools, 2, 2000, 1, added), -ENOBUFS);
  KUNIT_EXPECT_FALSE(test, is_memory_referenced(pools[0], 2000));
  KUNIT_EXPECT_FALSE(test, is_memory_referenced(pools[1], 2000));
  KUNIT_EXPECT_FALSE(test, added[0]);
  KUNIT_EXPECT_FALSE(test, added[1]);
  KUNIT_ASSERT_EQ(test, reference_memory_batch(pools, 2, 2000, 2, added), 0);
  KUNIT_EXPECT_TRUE(test, added[0]);
  KUNIT_EXPECT_TRUE(test, added[1]);
  KUNIT_EXPECT_EQ(test, pools[0]->mapped_num, 2u);
  KUNIT_EXPECT_EQ(test, pools[1]->mapped_num, 2u);
}

void test_case_memory_batch_existing_and_duplicate_pools(struct kunit * test)
{
  struct mempool_entry * first = assign_memory(1000);
  struct mempool_entry * second = assign_memory(1001);
  struct mempool_entry * third = assign_memory(1002);
  KUNIT_ASSERT_NOT_NULL(test, first);
  KUNIT_ASSERT_NOT_NULL(test, second);
  KUNIT_ASSERT_NOT_NULL(test, third);
  bool initial_added;
  KUNIT_ASSERT_EQ(test, reference_memory_batch(&first, 1, 2000, 1, &initial_added), 0);
  KUNIT_ASSERT_TRUE(test, initial_added);
  struct mempool_entry * pools[6] = {first, second, first, third, second, third};
  bool added[6] = {};
  KUNIT_ASSERT_EQ(test, reference_memory_batch(pools, 6, 2000, 1, added), -ENOBUFS);
  for (size_t i = 0; i < ARRAY_SIZE(added); ++i) KUNIT_EXPECT_FALSE(test, added[i]);
  KUNIT_EXPECT_EQ(test, first->mapped_num, 2u);
  KUNIT_EXPECT_EQ(test, second->mapped_num, 1u);
  KUNIT_EXPECT_EQ(test, third->mapped_num, 1u);
  KUNIT_ASSERT_EQ(test, reference_memory_batch(pools, 6, 2000, 2, added), 0);
  KUNIT_EXPECT_FALSE(test, added[0]);
  KUNIT_EXPECT_TRUE(test, added[1]);
  KUNIT_EXPECT_FALSE(test, added[2]);
  KUNIT_EXPECT_TRUE(test, added[3]);
  KUNIT_EXPECT_FALSE(test, added[4]);
  KUNIT_EXPECT_FALSE(test, added[5]);
  KUNIT_EXPECT_EQ(test, first->mapped_num, 2u);
  KUNIT_EXPECT_EQ(test, second->mapped_num, 2u);
  KUNIT_EXPECT_EQ(test, third->mapped_num, 2u);
  KUNIT_ASSERT_EQ(test, reference_memory_batch(pools, 6, 2000, 0, added), 0);
  for (size_t i = 0; i < ARRAY_SIZE(added); ++i) KUNIT_EXPECT_FALSE(test, added[i]);
}

void test_case_exited_publisher_pool_waits_for_subscriber_exit(struct kunit * test)
{
  // Restrict assignment to one already initialized pool. Restore the limit even
  // on allocation failure so the suite teardown still visits every pool.
  int saved_pool_num = mempool_num;
  struct mempool_entry * pool;
  bool added;
  int ret;

  mempool_num = 1;
  pool = assign_memory(1000);
  KUNIT_EXPECT_NOT_NULL(test, pool);
  if (!pool) goto restore;

  ret = reference_memory_batch(&pool, 1, 2000, 1, &added);
  KUNIT_EXPECT_EQ(test, ret, 0);
  if (ret) goto restore;
  KUNIT_EXPECT_TRUE(test, added);

  free_memory(1000);
  KUNIT_EXPECT_EQ(test, pool->mapped_num, 1u);
  KUNIT_EXPECT_TRUE(test, is_memory_referenced(pool, 2000));
  KUNIT_EXPECT_PTR_EQ(test, assign_memory(3000), NULL);

  free_memory(2000);
  KUNIT_EXPECT_EQ(test, pool->mapped_num, 0u);
  KUNIT_EXPECT_PTR_EQ(test, assign_memory(3000), pool);

restore:
  free_memory(1000);
  free_memory(2000);
  free_memory(3000);
  mempool_num = saved_pool_num;
}
