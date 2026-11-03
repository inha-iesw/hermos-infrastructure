// SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause
#include "hermos_internal.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(VERSION);

#ifndef KUNIT_BUILD
static int hermos_init(void)
{
  int ret;

  ret = hermos_init_device();
  if (ret < 0) return ret;

  ret = hermos_init_kthread();
  if (ret < 0) {
    hermos_exit_device();
    return ret;
  }

  ret = hermos_init_exit_hook();
  if (ret < 0) {
    hermos_exit_kthread();
    hermos_exit_device();
    return ret;
  }

  ret = init_memory_allocator();
  if (ret < 0) {
    hermos_exit_exit_hook();
    hermos_exit_kthread();
    hermos_exit_device();
    return ret;
  }

  dev_info(hermos_device, "Hermos installed! v%s\n", VERSION);
  return 0;
}

static void hermos_exit(void)
{
  hermos_exit_kthread();
  hermos_exit_exit_hook();

  hermos_exit_free_data();
  cleanup_memory_allocator();
  dev_info(hermos_device, "Hermos removed!\n");
  hermos_exit_device();
}

module_init(hermos_init) module_exit(hermos_exit)
#endif
