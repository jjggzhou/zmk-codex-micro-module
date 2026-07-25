#!/usr/bin/env python3
import hashlib
import pathlib
import re
import sys

EXPECTED_LENGTH = 240
EXPECTED_SHA256 = "2754bfba7fb82ba1d5070f926bdf35c92ef3868974916b04a4d3be34f4f1fad6"

data = pathlib.Path(sys.argv[1]).read_bytes()
digest = hashlib.sha256(data).hexdigest()
if len(data) != EXPECTED_LENGTH or digest != EXPECTED_SHA256:
    raise SystemExit(f"rejected: length={len(data)} sha256={digest}")
print(f"accepted behavior-compatible BLE map: length={len(data)} sha256={digest}")
if len(sys.argv) > 2:
    source = pathlib.Path(sys.argv[2]).read_text()
    declaration = source[source.index("const uint8_t codex_ble_report_map"):]
    initializer = declaration[declaration.index("{") + 1:declaration.index("};")]
    generated = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", initializer))
    if generated != data:
        raise SystemExit("rejected: C array differs from binary golden")
    print("accepted: C array equals binary golden")
