# Codex Micro BLE report map (behavior-compatible)

This file is a mechanically locked behavior-compatible map for ZMK v0.3.0. It
is not a capture of, and is not claimed byte-identical to, the original
216-byte Codex Micro BLE map.

It contains exactly the reports registered by the replacement HIDS service:
keyboard input and LED output (ID 1), consumer input (ID 2), mouse input (ID
3), plus 63-byte Vendor input/output/feature reports (ID 6). The mouse format
matches the pinned ZMK v0.3 `struct zmk_hid_mouse_report_body` ABI.

Length: 240 bytes

SHA-256: `2754bfba7fb82ba1d5070f926bdf35c92ef3868974916b04a4d3be34f4f1fad6`
