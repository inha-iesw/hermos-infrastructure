#!/bin/bash
#
# E2E regression smoke test for ros2hermos_discovery_agent.
#
# Starts one discovery agent in the current IPC namespace, lets it run for a
# couple of polling cycles, and verifies that an HermosDaemonState message
# appears on /_hermos_discovery_v2 with the expected fields populated. This
# does not require any Hermos pub / sub to be running -- the message is
# expected to carry an empty `topics` list in that case.
#
# Requires:
#   * hermos kernel module loaded (sudo insmod hermos_kmod/hermos.ko)
#   * workspace built (bash scripts/dev/build_all.bash)
#
# Usage:
#   bash scripts/test/e2e_test_discovery_agent.bash
#
# Exit codes:
#   0 - all checks passed
#   1 - prerequisite failure (kmod / workspace)
#   2 - assertion failure (msg fields didn't match expectation)

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
ECHO_TIMEOUT_SEC=${ECHO_TIMEOUT_SEC:-5}
DAEMON_WARMUP_SEC=${DAEMON_WARMUP_SEC:-2}

red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }

# ----- prerequisites -----
if ! grep -q "^hermos " /proc/modules; then
    red "ERROR: hermos kmod not loaded."
    echo "  -> sudo insmod $ROOT_DIR/hermos_kmod/hermos.ko" >&2
    exit 1
fi

if [ ! -f "$ROOT_DIR/install/setup.bash" ]; then
    red "ERROR: workspace not built ($ROOT_DIR/install/setup.bash missing)."
    echo "  -> bash $ROOT_DIR/scripts/dev/build_all.bash" >&2
    exit 1
fi

# shellcheck disable=SC1091
source "$ROOT_DIR/install/setup.bash"
set -u
green "✓ kmod loaded and workspace sourced"

# ----- start daemon -----
LOG_DIR=$(mktemp -d)
cleanup() {
    # pkill -P only reaches direct children; ros2 launch spawns its
    # talker / agent as grandchildren, so also match by binary path.
    pkill -P $$ 2>/dev/null || true
    pkill -KILL -f '/hermos_sample_application/lib/.*/talker' 2>/dev/null || true
    pkill -KILL -f '/ros2hermos_discovery_agent/lib/ros2hermos_discovery_agent/discovery_agent' 2>/dev/null || true
    sleep 1
    rm -rf "$LOG_DIR"
}
trap cleanup EXIT

yellow "Starting hermos_discovery_agent..."
ros2 run ros2hermos_discovery_agent discovery_agent > "$LOG_DIR/agent.log" 2>&1 &
sleep "$DAEMON_WARMUP_SEC"

if ! grep -q "discovery_agent up" "$LOG_DIR/agent.log"; then
    red "ERROR: daemon did not log startup within ${DAEMON_WARMUP_SEC}s."
    echo "----- agent log -----" >&2
    cat "$LOG_DIR/agent.log" >&2
    exit 2
fi
green "✓ daemon started"

# ----- assert msg shape -----
fail() {
    red "ERROR: $1"
    [ -n "${2:-}" ] && { echo "----- output -----" >&2; printf '%s\n' "$2" >&2; }
    echo "----- agent log -----" >&2
    cat "$LOG_DIR/agent.log" >&2
    exit 2
}

msg=$(timeout "$ECHO_TIMEOUT_SEC" ros2 topic echo --once /_hermos_discovery_v2 2>&1) \
    || fail "ros2 topic echo on /_hermos_discovery_v2 timed out or failed" "$msg"

grep -q '^schema_version: 2$'                       <<<"$msg" || fail "schema_version != 2" "$msg"
grep -q '^writer_role: 1$'                         <<<"$msg" || fail "writer is not a bridge agent" "$msg"
grep -qE '^device_id:.*[0-9]+:[0-9]+'              <<<"$msg" || fail "device identity missing" "$msg"
grep -q '^hermos_version: \(.*\)$'                <<<"$msg" || fail "hermos_version missing" "$msg"
# Assert hermos_version is non-empty so a misconfigured importlib.metadata
# lookup is caught (would silently fall back to '' otherwise).
grep -q '^hermos_version: ..*$'                   <<<"$msg" || fail "hermos_version is empty" "$msg"
grep -q '^host_uuid: [0-9a-f-]\+$'                  <<<"$msg" || fail "host_uuid missing or malformed" "$msg"
grep -q '^host_hostname: '                          <<<"$msg" || fail "host_hostname missing" "$msg"
grep -q '^ipc_ns_inode: [0-9]\+$'                   <<<"$msg" || fail "ipc_ns_inode missing or non-numeric" "$msg"
grep -q "^domain_id: ${ROS_DOMAIN_ID:-0}$"           <<<"$msg" || fail "domain_id does not match ROS_DOMAIN_ID" "$msg"
grep -qE "^session_id: ['\"]?[^[:space:]'\"]+"     <<<"$msg" || fail "session_id missing or empty" "$msg"
grep -qE '^snapshot_time_ns: [1-9][0-9]*$'          <<<"$msg" || fail "snapshot_time_ns missing or zero" "$msg"
grep -q '^lease_duration_ns: 30000000000$'          <<<"$msg" || fail "lease_duration_ns != 30 seconds" "$msg"
grep -q '^topics:'                                  <<<"$msg" || fail "topics field missing" "$msg"
green "✓ HermosDaemonState v2 shape OK (identity, domain, session, timestamp, lease, topics)"

# ----- QoS sanity (Reliable + TransientLocal + Liveliness Automatic) -----
qos=$(timeout "$ECHO_TIMEOUT_SEC" ros2 topic info /_hermos_discovery_v2 --verbose 2>&1) \
    || fail "ros2 topic info on /_hermos_discovery_v2 failed" "$qos"
grep -q "Reliability: RELIABLE"          <<<"$qos" || fail "QoS reliability != RELIABLE" "$qos"
grep -q "Durability: TRANSIENT_LOCAL"    <<<"$qos" || fail "QoS durability != TRANSIENT_LOCAL" "$qos"
grep -q "Liveliness: AUTOMATIC"          <<<"$qos" || fail "QoS liveliness != AUTOMATIC" "$qos"
green "✓ QoS profile: RELIABLE + TRANSIENT_LOCAL + Liveliness AUTOMATIC"

# ----- pop a talker in the same IPC NS and assert its endpoint shows up -----
yellow "Starting hermos_sample_application talker (discovery_agent=false to avoid a 2nd agent)..."
ros2 launch hermos_sample_application talker.launch.xml discovery_agent:=false \
    > "$LOG_DIR/talker.log" 2>&1 &
# Give the talker time to call its Publisher<T> ctor and the daemon time to
# observe it on its next 1 Hz tick.
sleep 3
msg_with_topic=$(timeout "$ECHO_TIMEOUT_SEC" ros2 topic echo --once /_hermos_discovery_v2 2>&1) \
    || fail "ros2 topic echo (with talker running) timed out" "$msg_with_topic"

grep -q 'topic_name: /my_topic'      <<<"$msg_with_topic" || fail "talker's /my_topic not seen in snapshot" "$msg_with_topic"
grep -q 'node_name: /talker_node'    <<<"$msg_with_topic" || fail "talker_node not seen in snapshot" "$msg_with_topic"
green "✓ talker's /my_topic + /talker_node appear in the snapshot"

# ----- assert gossip carries the ROS 2 message type from the tmpfs registry -----
# The agent reads the per-process tmpfs file written by hermoslib's
# Publisher<T> ctor and joins (topic, role, node_name, domain, pid) onto the
# kmod's ioctl output after validating process start ticks, so the gossip
# publication has type_name + pid filled in.
grep -q 'type_name: hermos_sample_interfaces/msg/DynamicSizeArray' <<<"$msg_with_topic" \
    || fail "gossip type_name not populated from the tmpfs registry" "$msg_with_topic"
# At least one HermosEndpoint.pid should be non-zero (the talker's pid the
# registry pinned to /talker_node).
grep -qE '^[[:space:]]+pid: [1-9][0-9]*' <<<"$msg_with_topic" \
    || fail "no HermosEndpoint.pid > 0 in snapshot (registry → gossip join failed)" "$msg_with_topic"
green "✓ gossip carries type_name + non-zero pid from the tmpfs type registry"

# ----- assert the tmpfs file the agent reads from is actually there -----
NS_INODE=$(stat -L -c '%i' /proc/self/ns/ipc)
TMPFS_DIR="${HERMOS_TMPFS_DIR:-/dev/shm}/hermos_type_registry/$NS_INODE"
[ -d "$TMPFS_DIR" ] || fail "tmpfs registry dir missing: $TMPFS_DIR"
# Match the talker's row rather than relying on another process's file mtime.
# Per-process format: <topic>\t<type>\t<role>\t<node>\t<domain>\t<start_ticks>\n.
registry_pattern=$'^/my_topic\thermos_sample_interfaces/msg/DynamicSizeArray\tpub\t/talker_node\t'"${ROS_DOMAIN_ID:-0}"$'\t[1-9][0-9]*$'
registry_file=$(grep -lE "$registry_pattern" "$TMPFS_DIR"/*.txt 2>/dev/null | head -1)
[ -n "$registry_file" ] \
    || fail "no talker registry row with the expected domain and start ticks in $TMPFS_DIR" "$(ls -la "$TMPFS_DIR" 2>&1)"
green "✓ tmpfs registry file present at $registry_file with the expected 6-field line"

green ""
green "===== ALL CHECKS PASSED ====="
