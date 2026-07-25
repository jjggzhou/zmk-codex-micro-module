#!/usr/bin/env python3
"""Accept only the original Codex Micro USB report descriptor capture."""

import hashlib
import pathlib
import sys

EXPECTED_LENGTH = 275
EXPECTED_SHA256 = "9257d7361f9c784e0fc0b260bbac0feadd49bf79cbb6202d6c41560cbae96fb6"


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} DESCRIPTOR")

    data = pathlib.Path(sys.argv[1]).read_bytes()
    actual_length = len(data)
    actual_sha256 = hashlib.sha256(data).hexdigest()

    if actual_length != EXPECTED_LENGTH:
        raise SystemExit(
            f"rejected: expected {EXPECTED_LENGTH} bytes, got {actual_length} bytes"
        )
    if actual_sha256 != EXPECTED_SHA256:
        raise SystemExit(
            f"rejected: expected SHA-256 {EXPECTED_SHA256}, got {actual_sha256}"
        )

    print(f"accepted: {actual_length} bytes, SHA-256 {actual_sha256}")


if __name__ == "__main__":
    main()
