#pragma once

#include <stddef.h>
#include <stdint.h>

#include <codex/descriptor.h>

#define CODEX_USB_VID UINT16_C(0x303A)
#define CODEX_USB_PID UINT16_C(0x8360)
#define CODEX_USB_BCD_DEVICE UINT16_C(0x0100)

uint16_t codex_usb_vid(void);
uint16_t codex_usb_pid(void);
uint16_t codex_usb_bcd_device(void);
const char *codex_usb_manufacturer(void);
const char *codex_usb_product(void);

const uint8_t *codex_usb_registered_report_descriptor(void);
int codex_usb_transport_init(void);
int codex_usb_send_vendor(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE]);
void codex_usb_transport_reset_writer(void);
int codex_usb_output_received(uint8_t report_id, const uint8_t *data, size_t len);

/*
 * Narrow Task 4 integration seam. A later transport queue provides strong
 * definitions; the Task 3 weak defaults deliberately do no parsing.
 */
int codex_usb_vendor_payload_received(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE]);
void codex_usb_bad_report_received(uint8_t report_id, size_t len);
