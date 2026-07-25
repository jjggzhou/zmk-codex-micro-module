#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zmk/hid.h>

void codex_test_usb_fakes_reset(void);

const struct device *codex_test_registered_device(void);
const uint8_t *codex_test_registered_descriptor(void);
size_t codex_test_registered_descriptor_size(void);
const struct hid_ops *codex_test_registered_ops(void);

const uint8_t *codex_test_last_write(void);
size_t codex_test_last_write_size(void);
size_t codex_test_write_attempt_count(void);
bool codex_test_write_in_flight(void);
void codex_test_complete_write(void);
void codex_test_set_mouse_report(struct zmk_hid_mouse_report report);
void codex_test_set_usb_status(enum usb_dc_status_code status);
size_t codex_test_wakeup_count(void);

size_t codex_test_received_count(void);
void codex_test_set_vendor_ingress_error(int error);
const uint8_t *codex_test_received_payload(void);
size_t codex_test_bad_report_count(void);
size_t codex_test_upstream_set_report_count(void);
size_t codex_test_upstream_in_ready_count(void);
uint8_t *codex_test_descriptor_block(void);
size_t codex_test_descriptor_block_size(void);
void codex_test_corrupt_endpoint_length(uint8_t length);
uint16_t codex_test_descriptor_vid(void);
const uint8_t *codex_test_manufacturer_utf16(void);
const uint8_t *codex_test_product_utf16(void);
const uint8_t *codex_test_serial_utf16(void);
const uint8_t *codex_test_descriptor_canary(void);
