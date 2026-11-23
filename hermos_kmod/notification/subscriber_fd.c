// SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause
#include "subscriber_fd.h"

#include <linux/anon_inodes.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

enum notify_state { NOTIFY_EMPTY, NOTIFY_READY, NOTIFY_DISCONNECTED };

struct hermos_subscriber_notify {
  wait_queue_head_t wait;
  atomic_t state;
  refcount_t refs;
};

static void put_notify(struct hermos_subscriber_notify *notify)
{
  if (refcount_dec_and_test(&notify->refs)) kfree(notify);
}

static void mark_ready(struct hermos_subscriber_notify *notify)
{
  if (atomic_cmpxchg(&notify->state, NOTIFY_EMPTY, NOTIFY_READY) == NOTIFY_EMPTY)
    wake_up_interruptible_poll(&notify->wait, EPOLLIN | EPOLLRDNORM);
}

static __poll_t notify_poll(struct file *file, poll_table *table)
{
  struct hermos_subscriber_notify *notify = file->private_data;
  poll_wait(file, &notify->wait, table);
  switch (atomic_read(&notify->state)) {
    case NOTIFY_READY:
      return EPOLLIN | EPOLLRDNORM;
    case NOTIFY_DISCONNECTED:
      return EPOLLHUP;
    default:
      return 0;
  }
}

ssize_t hermos_take_subscriber_notify(struct hermos_subscriber_notify *notify)
{
  switch (atomic_cmpxchg(&notify->state, NOTIFY_READY, NOTIFY_EMPTY)) {
    case NOTIFY_DISCONNECTED:
      return 0;
    case NOTIFY_EMPTY:
      return -EAGAIN;
    default:
      return sizeof(u64);
  }
}

static ssize_t notify_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
  struct hermos_subscriber_notify *notify = file->private_data;
  const u64 token = 1;
  if (!count) return 0;
  if (count < sizeof(token)) return -EINVAL;
  ssize_t available = hermos_take_subscriber_notify(notify);
  if (available <= 0) return available;
  if (copy_to_user(buf, &token, sizeof(token))) {
    mark_ready(notify);
    return -EFAULT;
  }
  return sizeof(token);
}

static int notify_release(struct inode *inode, struct file *file)
{
  put_notify(file->private_data);
  return 0;
}

static const struct file_operations notify_ops = {
  .owner = THIS_MODULE,
  .poll = notify_poll,
  .read = notify_read,
  .release = notify_release,
};

struct file *hermos_new_subscriber_notify_file(struct hermos_subscriber_notify **out)
{
  struct hermos_subscriber_notify *notify = kmalloc(sizeof(*notify), GFP_KERNEL);
  if (!notify) return ERR_PTR(-ENOMEM);
  init_waitqueue_head(&notify->wait);
  /* A publish may have committed between subscriber registration and this bind. */
  atomic_set(&notify->state, NOTIFY_READY);
  refcount_set(&notify->refs, 2);
  struct file *file = anon_inode_getfile(
    "[hermos-subscriber-notify]", &notify_ops, notify, O_RDONLY | O_NONBLOCK);
  if (IS_ERR(file)) {
    kfree(notify);
    return file;
  }
  *out = notify;
  return file;
}

void hermos_signal_subscriber_notify(struct hermos_subscriber_notify *notify)
{
  /* Registration and PUBLISH have already checked the namespace owner. */
  mark_ready(notify);
}

void hermos_disconnect_subscriber_notify(struct hermos_subscriber_notify *notify)
{
  if (!notify) return;
  atomic_set(&notify->state, NOTIFY_DISCONNECTED);
  wake_up_interruptible_poll(&notify->wait, EPOLLHUP);
  put_notify(notify);
}
