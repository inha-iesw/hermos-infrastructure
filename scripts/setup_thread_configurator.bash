#!/bin/bash

# Set up the hermos CIE thread configurator.
# Reference: https://autowarefoundation.github.io/agnocast_doc/callback-isolated-executor/integration-guide/#step-2-set-up-the-thread-configurator
#
# Usage:
#   source /opt/ros/<distro>/setup.bash
#   source <root-owned-install-prefix>/setup.bash
#   ./scripts/setup_thread_configurator.bash
#
# This script automates:
#   1. Granting CAP_SYS_NICE to the thread_configurator_node binary.
#   2. Registering required library paths in /etc/ld.so.conf.d/hermos-cie.conf.

set -euo pipefail

# Failed validation must not hide loader paths installed by an older version.
conf_file=/etc/ld.so.conf.d/hermos-cie.conf
warn_existing_configuration() {
    local status=$?
    if (( status != 0 )) && [[ -f "$conf_file" ]]; then
        echo "Warning: existing loader configuration remains at $conf_file" >&2
        echo "Review it; to remove it, run: sudo rm -- $conf_file && sudo ldconfig" >&2
    fi
}
trap warn_existing_configuration EXIT

# --- Prerequisites ---------------------------------------------------------

if [ -z "${ROS_DISTRO:-}" ]; then
	echo "Error: ROS_DISTRO is not set. Please source your ROS 2 environment first:"
	echo "  source /opt/ros/<distro>/setup.bash"
	exit 1
fi

if ! command -v ros2 >/dev/null 2>&1; then
	echo "Error: 'ros2' command not found. Please source your ROS 2 environment first:"
	echo "  source /opt/ros/\$ROS_DISTRO/setup.bash"
	exit 1
fi

if ! command -v dpkg-architecture >/dev/null 2>&1; then
	echo "Error: 'dpkg-architecture' not found. Install it with: sudo apt-get install dpkg-dev"
	exit 1
fi

if ! command -v getcap >/dev/null 2>&1; then
	echo "Error: 'getcap' not found. Install it with: sudo apt-get install libcap2-bin"
	exit 1
fi

if ! command -v setcap >/dev/null 2>&1; then
	echo "Error: 'setcap' not found. Install it with: sudo apt-get install libcap2-bin"
	exit 1
fi

if ! command -v ldconfig >/dev/null 2>&1; then
	echo "Error: 'ldconfig' not found. Install it with: sudo apt-get install libc-bin"
	exit 1
fi

if ! thread_configurator_prefix=$(ros2 pkg prefix hermos_cie_thread_configurator 2>/dev/null); then
	echo "Error: Package 'hermos_cie_thread_configurator' not found."
	echo "       Install into a root-owned prefix and source its setup.bash first."
	exit 1
fi

if ! config_msgs_prefix=$(ros2 pkg prefix hermos_cie_config_msgs 2>/dev/null); then
	echo "Error: Package 'hermos_cie_config_msgs' not found."
	echo "       Install into a root-owned prefix and source its setup.bash first."
	exit 1
fi

# Validate all trusted loader inputs before the first privileged operation.
# File capabilities suppress LD_LIBRARY_PATH; adding a writable workspace to
# ldconfig would instead make its libraries trusted by privileged programs.
require_trusted_path() {
    local current owner mode
    current=$(readlink -e -- "$1") || {
        echo "Error: missing trusted installation path: $1"
        exit 1
    }
    while true; do
        read -r owner mode < <(stat -c '%u %a' -- "$current")
        if [[ "$owner" != 0 ]] || (( (8#$mode & 0022) != 0 )); then
            echo "Error: untrusted installation path: $current"
            echo "Install the configurator and dependencies under a root-owned prefix"
            echo "with no group/other-writable files or ancestor directories, then rerun."
            exit 1
        fi
        [[ "$current" == / ]] && break
        current=$(dirname -- "$current")
    done
}

bin_path=$(readlink -f "${thread_configurator_prefix}/lib/hermos_cie_thread_configurator/thread_configurator_node")
if [ ! -f "$bin_path" ]; then
    echo "Error: binary not found at: $bin_path"
    exit 1
fi
ros_lib_dir="/opt/ros/${ROS_DISTRO}/lib"
ros_multiarch_lib_dir="$ros_lib_dir/$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
library_dirs=("$ros_lib_dir" "${config_msgs_prefix}/lib" "${thread_configurator_prefix}/lib")
if [[ -d "$ros_multiarch_lib_dir" ]]; then
    library_dirs+=("$ros_multiarch_lib_dir")
fi
require_trusted_path "$bin_path"
for index in "${!library_dirs[@]}"; do
    # Persist the checked canonical path, not an alias a caller can retarget.
    canonical_library_dir=$(readlink -e -- "${library_dirs[$index]}") || {
        echo "Error: missing library path: ${library_dirs[$index]}"
        exit 1
    }
    library_dirs[$index]=$canonical_library_dir
    library_dir="${library_dirs[$index]}"
    require_trusted_path "$library_dir"
    # --symlink-install can leave a trusted directory pointing into a writable
    # build tree. Check resolved library targets as well as the directory.
    while IFS= read -r -d '' library; do
        require_trusted_path "$library"
    done < <(find -L "$library_dir" -maxdepth 1 -name '*.so*' -print0)
done

# --- Step 1: Grant capabilities --------------------------------------------

echo "[1/2] Grant CAP_SYS_NICE to thread_configurator_node"

current_caps=$(getcap "$bin_path" 2>/dev/null || true)
if echo "$current_caps" | grep -q "cap_sys_nice=eip"; then
	echo "  Already set ($current_caps). Skipping."
else
	echo "  Target: $bin_path"
	sudo setcap cap_sys_nice=eip "$bin_path"
	echo "  Done: $(getcap "$bin_path")"
fi

# --- Step 2: Configure library paths ---------------------------------------

echo "[2/2] Configure library paths (/etc/ld.so.conf.d/hermos-cie.conf)"

expected_content=$(printf '%s\n' "${library_dirs[@]}")

if [ -f "$conf_file" ] && [ "$(cat "$conf_file")" = "$expected_content" ]; then
	echo "  Already configured ($conf_file). Skipping."
else
	echo "  Writing: $conf_file"
	echo "  Content:"
	printf '    %s\n' "${library_dirs[@]}"
	printf '%s\n' "${library_dirs[@]}" | sudo tee "$conf_file" >/dev/null
	sudo ldconfig
	echo "  Done."
fi

echo "All steps completed."
