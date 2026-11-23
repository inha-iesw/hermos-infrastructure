/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
#pragma once

#include <linux/types.h>

struct file;
struct hermos_subscriber_notify;

/* The subscriber and the returned file hold independent references. */
struct file *hermos_new_subscriber_notify_file(struct hermos_subscriber_notify **out);
ssize_t hermos_take_subscriber_notify(struct hermos_subscriber_notify *notify);
void hermos_signal_subscriber_notify(struct hermos_subscriber_notify *notify);
void hermos_disconnect_subscriber_notify(struct hermos_subscriber_notify *notify);
