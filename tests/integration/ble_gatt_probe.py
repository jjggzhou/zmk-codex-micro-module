#!/usr/bin/env python3
"""Offline probe of the real linked nRF52 ELF; this is not a live BLE probe."""

import argparse
import json
import pathlib
import subprocess
import struct

from elftools.elf.elffile import ELFFile


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", type=pathlib.Path)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    elf_path = build / "zephyr" / "zmk.elf"
    commands = json.loads((build / "compile_commands.json").read_text())
    sources = [entry["file"] for entry in commands]
    upstream = [p for p in sources if p.endswith("/zmk/app/src/hog.c")]
    replacement = [p for p in sources if p.endswith("/src/transport/ble_hids.c")]
    assert not upstream, f"upstream hog.c still compiled: {upstream}"
    assert len(replacement) == 1, f"replacement count={len(replacement)}"

    objdump = "/opt/zephyr-sdk-0.16.3/arm-zephyr-eabi/bin/arm-zephyr-eabi-objdump"
    disassembly = subprocess.run(
        [objdump, "-d", str(elf_path)], check=True, capture_output=True, text=True
    ).stdout

    def function_body(name: str) -> str:
        marker = f"<{name}>:"
        start = disassembly.index(marker)
        end = disassembly.find("\n\n", start)
        return disassembly[start:] if end < 0 else disassembly[start:end]

    mouse_sender = function_body("zmk_endpoints_send_mouse_report")
    mouse_wrapper = function_body("__wrap_zmk_usb_hid_send_mouse_report")
    assert "<__wrap_zmk_usb_hid_send_mouse_report>" in mouse_sender
    assert "<__wrap_hid_int_ep_write>" in mouse_wrapper

    usb_init = function_body("zmk_usb_init")
    usb_wrapper = function_body("__wrap_usb_enable")
    usb_status_callback = function_body("codex_usb_status_callback")
    ble_notify = function_body("codex_ble_vendor_notify")
    assert "<__wrap_usb_enable>" in usb_init
    assert "<usb_enable>" in usb_wrapper
    assert "<codex_router_on_usb_physical_state>" in usb_status_callback
    assert "<bt_gatt_notify_cb>" in ble_notify

    config = (build / "zephyr" / ".config").read_text().splitlines()
    wanted = {
        "CONFIG_BT_L2CAP_TX_MTU=69",
        "CONFIG_BT_BUF_ACL_RX_SIZE=73",
        "CONFIG_BT_BUF_ACL_TX_SIZE=73",
        "CONFIG_BT_GATT_AUTO_UPDATE_MTU=y",
        "CONFIG_ZMK_POINTING=y",
        "# CONFIG_ZMK_POINTING_SMOOTH_SCROLLING is not set",
    }
    missing = wanted.difference(config)
    assert not missing, f"configuration contract missing: {sorted(missing)}"

    with elf_path.open("rb") as stream:
        elf = ELFFile(stream)
        symtab = elf.get_section_by_name(".symtab")
        symbols = {sym.name: sym for sym in symtab.iter_symbols() if sym.name}
        assert "vendor_notify_msgq" not in symbols
        assert "vendor_notify_work" not in symbols

        def data_at(address: int, size: int) -> bytes:
            for segment in elf.iter_segments():
                start = segment["p_vaddr"]
                end = start + segment["p_filesz"]
                if start <= address and address + size <= end:
                    return segment.data()[address - start : address - start + size]
            raise AssertionError(f"ELF address 0x{address:x}+{size} has no file data")

        def uuid16(address: int) -> int:
            raw = data_at(address, 4)
            assert raw[0] == 0, f"UUID at 0x{address:x} is not 16-bit"
            return struct.unpack_from("<H", raw, 2)[0]

        assert symbols["codex_ble_report_map"]["st_size"] == 240
        assert symbols["attr_hog_svc"]["st_size"] == 32 * 20
        assert symbols["hog_svc"]["st_size"] == 8

        service_section = next(
            section for section in elf.iter_sections()
            if section.name.startswith("bt_gatt_service_static")
        )
        services = service_section.data()
        assert len(services) % 8 == 0
        hids_count = 0
        for offset in range(0, len(services), 8):
            attrs, count = struct.unpack_from("<II", services, offset)
            assert count > 0
            first = data_at(attrs, 20)
            first_uuid = struct.unpack_from("<I", first, 0)[0]
            service_uuid = struct.unpack_from("<I", first, 12)[0]
            if uuid16(first_uuid) == 0x2800 and uuid16(service_uuid) == 0x1812:
                hids_count += 1
        assert hids_count == 1, f"HIDS primary service count={hids_count}"

        attr_symbol = symbols["attr_hog_svc"]
        attrs = data_at(attr_symbol["st_value"], attr_symbol["st_size"])
        parsed = []
        for offset in range(0, len(attrs), 20):
            uuid_ptr, read, write, user_data, handle, perm = struct.unpack_from("<IIIIHH", attrs, offset)
            parsed.append((uuid16(uuid_ptr), read, write, user_data, handle, perm))

        refs = []
        for index, attr in enumerate(parsed):
            if attr[0] != 0x2908:
                continue
            report_id, report_type = data_at(attr[3], 2)
            refs.append((report_id, report_type))
            value_index = index - 2 if report_type == 1 else index - 1
            declaration_index = value_index - 1
            assert parsed[value_index][0] == 0x2A4D
            assert parsed[declaration_index][0] == 0x2803
            properties = data_at(parsed[declaration_index][3], 7)[6]
            if report_type == 1:
                assert parsed[index - 1][0] == 0x2902, "input report missing CCC"
                assert parsed[value_index][5] == 0x04, "input value is not read-encrypted"
                assert properties == 0x12, "input is not read+notify"
            else:
                assert parsed[value_index][5] == 0x0C, "writable value is not encrypted"
                assert properties == 0x0E, "output/feature is not read+write+write-cmd"

        expected_refs = [(1, 1), (2, 1), (3, 1), (1, 2), (6, 1), (6, 2), (6, 3)]
        assert sorted(refs) == sorted(expected_refs), f"Report References={refs}"

    print("offline ELF probe: one HIDS, 32 attrs, exact refs/CCC/encryption, auto MTU exchange")
    print("USB mouse endpoint calls the ABI converter, which calls the shared serialized HID writer")
    print("USB enable is wrapped for physical edges; BLE notify calls GATT directly with no notify work queue")
    print("physical live GATT round-trip is intentionally deferred to Task 16")


if __name__ == "__main__":
    main()
