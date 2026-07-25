#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

void codex_test_usb_fakes_reset(void);

const struct device *codex_test_registered_device(void);
const uint8_t *codex_test_registered_descriptor(void);
size_t codex_test_registered_descriptor_size(void);
const struct hid_ops *codex_test_registered_ops(void);

const uint8_t *codex_test_last_write(void);
size_t codex_test_last_write_size(void);

size_t codex_test_received_count(void);
const uint8_t *codex_test_received_payload(void);
size_t codex_test_bad_report_count(void);
size_t codex_test_upstream_set_report_count(void);
const uint8_t *codex_test_manufacturer_utf16(void);
const uint8_t *codex_test_product_utf16(void);
