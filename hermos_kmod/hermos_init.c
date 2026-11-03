// SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause
#include "hermos_internal.h"

// =========================================
// Initialize and cleanup

#if KERNEL_VERSION(6, 2, 0) <= LINUX_VERSION_CODE
static char * hermos_devnode(const struct device * dev, umode_t * mode)
#else
static char * hermos_devnode(struct device * dev, umode_t * mode)
#endif
{
  if (mode) {
    *mode = 0666;
  }
  return NULL;
}

static const struct file_operations fops = {
  .owner = THIS_MODULE,
  .unlocked_ioctl = hermos_ioctl,
};

static int exit_worker_thread(void * data)
{
  while (!kthread_should_stop()) {
    // Pairs with smp_store_release() in hermos_enqueue_exit_pid(); ensures all
    // queue writes from the enqueuer are visible before we read the queue.
    wait_event_interruptible(worker_wait, smp_load_acquire(&has_new_pid) || kthread_should_stop());

    if (kthread_should_stop()) break;

    // Drain all queued PIDs in a single wake-up cycle
    while (true) {
      pid_t pid;
      unsigned long flags;
      bool got_pid = false;

      spin_lock_irqsave(&pid_queue_lock, flags);

      if (queue_head != queue_tail) {
        pid = exit_pid_queue[queue_head];
        queue_head = (queue_head + 1) & EXIT_QUEUE_MASK;
        got_pid = true;
      }

      // Release ensures the queue-drain reads complete before has_new_pid is cleared,
      // preventing the enqueuer from seeing a stale 0 after re-checking the queue.
      if (queue_head == queue_tail) smp_store_release(&has_new_pid, 0);

      spin_unlock_irqrestore(&pid_queue_lock, flags);

      if (!got_pid) break;

      hermos_process_exit_cleanup(pid);
    }
  }

  return 0;
}

static void find_sched_process_exit_tp(struct tracepoint * tp, void * priv)
{
  if (strcmp(tp->name, "sched_process_exit") == 0) {
    tp_sched_process_exit = tp;
  }
}

int hermos_init_device(void)
{
  int ret;

  // Device, class and driver names follow the module name so that a renamed build
  // (HERMOS_MODULE_NAME in the Makefile) can be loaded next to another Hermos kmod.
  major = register_chrdev(0, KBUILD_MODNAME /*device driver name*/, &fops);
  if (major < 0) {
    pr_err("hermos: register_chrdev failed: %d\n", major);
    return major;
  }

#if KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE
  hermos_class = class_create(KBUILD_MODNAME "_class");
#else
  hermos_class = class_create(THIS_MODULE, KBUILD_MODNAME "_class");
#endif
  if (IS_ERR(hermos_class)) {
    ret = PTR_ERR(hermos_class);
    pr_err("hermos: class_create failed: %d\n", ret);
    goto err_unregister_chrdev;
  }

  hermos_class->devnode = hermos_devnode;
  hermos_device =
    device_create(hermos_class, NULL, MKDEV(major, 0), NULL, KBUILD_MODNAME /*file name*/);
  if (IS_ERR(hermos_device)) {
    ret = PTR_ERR(hermos_device);
    pr_err("hermos: device_create failed: %d\n", ret);
    goto err_class_destroy;
  }

  return 0;

err_class_destroy:
  class_destroy(hermos_class);
err_unregister_chrdev:
  unregister_chrdev(major, KBUILD_MODNAME);
  return ret;
}

int hermos_init_kthread(void)
{
  queue_head = queue_tail = 0;

  worker_task = kthread_run(exit_worker_thread, NULL, "hermos_exit_worker");
  if (IS_ERR(worker_task)) {
    dev_warn(hermos_device, "failed to create kernel thread. (%s)\n", __func__);
    return PTR_ERR(worker_task);
  }

  return 0;
}

int hermos_init_exit_hook(void)
{
  int ret;

  for_each_kernel_tracepoint(find_sched_process_exit_tp, NULL);
  if (!tp_sched_process_exit) {
    dev_warn(hermos_device, "sched_process_exit tracepoint not found\n");
    return -ENOENT;
  }

  ret = tracepoint_probe_register(tp_sched_process_exit, hermos_process_exit, NULL);
  if (ret) {
    dev_warn(hermos_device, "tracepoint_probe_register failed: %d\n", ret);
    return ret;
  }

  return 0;
}
