#!/usr/bin/env python3
"""Prove Codex production sources are absent when CONFIG_CODEX_MICRO=n."""

import argparse
import json
import pathlib


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", type=pathlib.Path)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    config = (build / "zephyr" / ".config").read_text().splitlines()
    assert "# CONFIG_CODEX_MICRO is not set" in config

    commands = json.loads((build / "compile_commands.json").read_text())
    sources = {pathlib.PurePosixPath(entry["file"]).as_posix() for entry in commands}
    forbidden_suffixes = (
        "/src/descriptors/ble_report_map.c",
        "/src/transport/ble_queue.c",
        "/src/transport/ble_report_state.c",
        "/src/transport/ble_hids.c",
        "/src/transport/report_router.c",
        "/src/input/codex_keys.c",
        "/src/input/encoder.c",
        "/src/input/activity_bridge.c",
        "/src/input/analog_stick.c",
        "/src/input/touch_control.c",
        "/src/state/layers.c",
        "/src/state/connection_mode.c",
        "/src/state/indicators.c",
        "/src/lighting/model.c",
        "/src/lighting/effects.c",
        "/src/lighting/renderer.c",
    )
    found = sorted(source for source in sources if source.endswith(forbidden_suffixes))
    found.extend(sorted(source for source in sources if "codex_zmk_activity_" in source))
    assert not found, f"Codex production sources leaked into non-Codex build: {found}"
    print("non-Codex compile probe: CONFIG_CODEX_MICRO=n and zero Codex production sources")


if __name__ == "__main__":
    main()
