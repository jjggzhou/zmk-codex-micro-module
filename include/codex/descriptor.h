#pragma once

#include <stddef.h>
#include <stdint.h>

#define CODEX_VENDOR_REPORT_ID UINT8_C(0x06)
#define CODEX_VENDOR_PAYLOAD_SIZE 63U
#define CODEX_INTERNAL_REPORT_SIZE 64U

extern const uint8_t codex_usb_report_descriptor[275];

size_t codex_usb_report_descriptor_size(void);
