# Subscriber notifications

Each callback subscription registers one pollable descriptor with the Hermos
executor. On the current driver, `HERMOS_OPEN_SUBSCRIBER_NOTIFY_FD_CMD` binds
an anonymous fd to the subscription. It starts readable for the initial receive
check. Publishing marks eligible subscribers ready inside the publish ioctl.
The executor acknowledges readiness with a `uint64_t` read and calls
`HERMOS_RECEIVE_MSG_CMD` to obtain the available message references. Multiple
publications can share one wakeup. Removing the subscription disconnects the fd.

## POSIX MQ compatibility

A module without the notification-fd command returns `EINVAL` or `ENOTTY`.
For this fallback, registration must return a slash-prefixed token containing
the 32 hexadecimal digits of the module load nonce, `-`, and the 16 hexadecimal
digits of the topic generation. The library reports MQ fallback at subscription
creation. A module returning a logical ROS topic name is incompatible and
registration fails with `EPROTONOSUPPORT`. Other fd-bind errors propagate.
The `-` separator cannot occur in a valid ROS topic name. Use valid ROS names
for both sides of a domain bridge rule, including aliases supplied in YAML.
Several builds report version 2.3.5; check the deployed artifact identity.

The fallback subscriber creates `/hermos@<nonce>-<generation>@<subscriber-id>`
with `O_EXCL` and mode 0600. Domain bridge endpoints sharing one underlying
topic use the same generation. Recreating an independent topic gives it a new
generation. A publisher opens the corresponding owner-only queue and sends a
zero-length notification. The single slot coalesces repeated notifications until
the executor drains it and receives via ioctl. The current kernel module can
serve fd subscribers and token-based MQ subscribers on the same topic.

Take-only subscriptions receive on demand. CPU payloads and GPU descriptors
continue to be accessed through their existing shared-memory lifetime contract.
