#!/usr/bin/env bash
# This fixture must never build: source channels belong to a different producer.
set -euo pipefail

: "${ZEPHYR_BASE:?set ZEPHYR_BASE to the project-local Zephyr checkout}"
: "${ZMK_ANALOG_INPUT_DRIVER:?set ZMK_ANALOG_INPUT_DRIVER to the pinned driver module}"

module_dir="$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/codex-analog-invalid.XXXXXX")"
log_file="$build_dir/build.log"

set +e
west build -p always -s "$module_dir/tests/analog_invalid" -d "$build_dir" -b native_posix_64 -- \
    -DZephyr_DIR="$ZEPHYR_BASE/share/zephyr-package/cmake" \
    -DZEPHYR_EXTRA_MODULES="$module_dir;$ZMK_ANALOG_INPUT_DRIVER" >"$log_file" 2>&1
status=$?
set -e

if [ "$status" -eq 0 ]; then
    echo "invalid source-parent fixture unexpectedly built; log: $log_file" >&2
    exit 1
fi

grep -F "analog source channels must be children of input-device" "$log_file"
echo "invalid source-parent fixture rejected as expected; log: $log_file"
