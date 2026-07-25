#!/usr/bin/env python3
import pathlib
import sys

output = pathlib.Path(sys.argv[1])
paths = [pathlib.Path(arg) for arg in sys.argv[2:]]

lines = ["#pragma once", ""]
for index, path in enumerate(paths):
    data = path.read_bytes()
    octets = ", ".join(f"0x{byte:02x}" for byte in data)
    lines.append(f"static const uint8_t corpus_data_{index}[] = {{{octets}}};")
lines.extend([
    "",
    "static const struct corpus_case corpus_cases[] = {",
])
for index, path in enumerate(paths):
    expected_valid = "true" if path.name.startswith("valid_") else "false"
    lines.append(
        f'    {{"{path.name}", corpus_data_{index}, sizeof(corpus_data_{index}), '
        f"{expected_valid}}},"
    )
lines.extend(["};", ""])
output.write_text("\n".join(lines), encoding="utf-8")
