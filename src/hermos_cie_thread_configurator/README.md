# Thread configurator deployment and failure contract

The configurator uses `CAP_SYS_NICE` to apply configured scheduling policy,
nice values and affinity to announced local threads. A configuration name is
not an authentication token.

Non-ROS datagram announcements retain the existing socket reachability. Linux
`SCM_CREDENTIALS` supplies the sender process ID, and the listener accepts a
positive `pid_t` thread ID only when `/proc/<tid>/status` identifies that sender
as its thread group. Invalid, missing-credential or foreign-thread announcements
are logged and dropped before invoking the configurator. Only the configurator effective UID may announce its own threads. Root does
not implicitly accept another UID through SUDO_UID; run the configurator as the
workload owner. This receive-time check does not authenticate DDS requests or
make the later numeric-TID scheduling operation atomic.

DDS `CallbackGroupInfo` contains only a callback group name and thread ID. It
has no authenticated local process identity, so the configurator still requires
trusted DDS publishers and domain access controls. A numeric thread ID can also
be recycled after any receive-time check. Stored IDs used by `reapply_config`
are not stable task handles and are not authenticated again against their
original owner. Do not expose the configurator to untrusted DDS participants or
use stored-thread reapplication as a cross-user authorization boundary. Solving
those limitations requires a separate authenticated ownership/lifecycle contract;
changing only the datagram listener does not solve them.

The setup script rejects executable/library paths with non-root ownership or
group/other-writable components before running `sudo`. Library symlink targets
must satisfy the same rule. Install the configurator and its dependencies into
a trusted system prefix first. An ordinary user-owned workspace or symlink
installation is not suitable for the global loader cache or file capabilities.
The script does not relocate a workspace or change its ownership.

`SCHED_DEADLINE` affinity currently requires the cgroup-v1 cpuset controller
and filesystem permission to create child groups; `CAP_SYS_NICE` alone does
not grant that filesystem access.
Unavailable controllers, existing group paths and failed cpuset preparation reject
DEADLINE configuration without changing scheduling policy. Created group names
are process-specific, and cleanup only visits groups created by this node. CPU
indices outside the fixed `cpu_set_t` range are rejected for policies using
`sched_setaffinity`; cgroup cpusets retain their larger CPU-index support.

For DEADLINE with affinity, the node prepares the cpuset CPU and memory masks,
saves the existing scheduling attributes, applies DEADLINE, then moves the
thread into the cpuset. This order preserves the broad CPU mask required by
DEADLINE admission. If moving the thread fails, the node restores the saved
policy and removes its prepared group; a restore failure is reported separately.
The kernel may reject a later DEADLINE reconfiguration if the thread is already
in a restricted cpuset that does not span its scheduling root domain.

Other policies still apply affinity before scheduling policy. Those sequences
are not transactional: a subsequent scheduler or nice-value failure can leave
an earlier affinity or policy change applied. The configuration remains
unapplied and the error is reported. No real-time or cgroup operations are
performed by the host unit tests; cgroup-v1 device validation is still required.
