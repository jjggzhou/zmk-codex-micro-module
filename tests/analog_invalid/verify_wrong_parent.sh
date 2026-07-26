#!/usr/bin/env bash
# These fixtures must never build: producer identity and child-set contracts are strict.
set -euo pipefail

: "${ZEPHYR_BASE:?set ZEPHYR_BASE to the project-local Zephyr checkout}"
: "${ZMK_ANALOG_INPUT_DRIVER:?set ZMK_ANALOG_INPUT_DRIVER to the pinned driver module}"

module_dir="$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)"
verify_invalid_fixture() {
    local fixture_name="$1"
    local overlay="$2"
    local expected_assertion="$3"
    local build_dir
    local log_file
    local status

    build_dir="$(mktemp -d "${TMPDIR:-/tmp}/codex-analog-invalid.XXXXXX")"
    log_file="$build_dir/build.log"
    set +e
    west build -p always -s "$module_dir/tests/analog_invalid" -d "$build_dir" -b native_posix_64 -- \
        -DZephyr_DIR="$ZEPHYR_BASE/share/zephyr-package/cmake" \
        -DZEPHYR_EXTRA_MODULES="$module_dir;$ZMK_ANALOG_INPUT_DRIVER" \
        -DDTC_OVERLAY_FILE="$overlay" >"$log_file" 2>&1
    status=$?
    set -e
    if [ "$status" -eq 0 ]; then
        echo "invalid $fixture_name fixture unexpectedly built; log: $log_file" >&2
        exit 1
    fi
    grep -F "$expected_assertion" "$log_file"
    echo "invalid $fixture_name fixture rejected as expected; log: $log_file"
}

verify_invalid_fixture "source-parent" \
    "$module_dir/tests/analog_invalid/native_posix_64.overlay" \
    "analog source channels must be children of input-device"
verify_invalid_fixture "third-child" \
    "$module_dir/tests/analog_invalid/extra_third_child.overlay" \
    "analog input-device must have exactly source-x-channel and source-y-channel children"
