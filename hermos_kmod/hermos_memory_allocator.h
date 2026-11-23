/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
#pragma once

#include <linux/list.h>
#include <linux/types.h>

// Default is 4096, can be overridden by insmod parameter mempool_num
extern int mempool_num;
// Default is 0x40000000000, can be overridden by insmod parameter mempool_start_addr
extern unsigned long mempool_start_addr;
// Default is 16GB, can be overridden by insmod parameter mempool_size_gb
extern int mempool_size_gb;
// Mempool size in bytes (calculated from mempool_size_gb)
extern uint64_t mempool_size_bytes;

struct mapped_pid_entry
{
  pid_t pid;
  struct list_head list;
};

struct mempool_entry
{
  uint64_t addr;
  uint32_t mapped_num;
  struct list_head mapped_pid_head;
};

int init_memory_allocator(void);
void cleanup_memory_allocator(void);
struct mempool_entry * assign_memory(const pid_t pid);
/**
 * @brief Query an existing process mapping without changing its lifetime.
 * @param pool Non-null live pool; caller must prevent pool reuse/reference removal.
 * @param pid Process whose mapping is queried.
 * @return True if the mapping already exists.
 */
bool is_memory_referenced(struct mempool_entry * pool, pid_t pid);
/**
 * @brief Atomically add a process to a batch of pool mappings.
 * @param pools Non-null array of count live pools; the caller keeps the pools alive.
 * @param count Number of pool entries, possibly including duplicates.
 * @param pid Process acquiring the mappings.
 * @param capacity Maximum number of new mappings the caller can report.
 * @param added Non-null output array of count flags; true only for newly added mappings.
 * @return Zero on success, or a negative errno with no mappings changed.
 */
int reference_memory_batch(
  struct mempool_entry * const * pools, uint32_t count, pid_t pid, uint32_t capacity,
  bool * added);
void free_memory(const pid_t pid);
void exit_memory_allocator(void);
