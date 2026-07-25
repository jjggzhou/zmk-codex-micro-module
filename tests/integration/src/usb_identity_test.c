#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/ztest.h>

#include <codex/descriptor.h>
#include <codex/usb.h>

#include "usb_fakes.h"

#define HID_REPORT_TYPE_OUTPUT UINT16_C(0x0200)

extern uint8_t *usb_get_device_descriptor(void);
extern void codex_test_upstream_set_report_called(void);

static const struct device *const fake_hid_device =
    (const struct device *)(uintptr_t)UINT32_C(0x1234);

static int upstream_set_report(const struct device *dev, struct usb_setup_packet *setup,
                               int32_t *len, uint8_t **data)
{
    (void)dev;
    (void)setup;
    (void)len;
    (void)data;
    codex_test_upstream_set_report_called();
    return -ENOTSUP;
}

static const struct hid_ops upstream_ops = {
    .set_report = upstream_set_report,
};

static void usb_identity_before(void *fixture)
{
    static const uint8_t rejected_descriptor[] = {0x00};

    (void)fixture;
    codex_test_usb_fakes_reset();
    usb_hid_register_device(fake_hid_device, rejected_descriptor, sizeof(rejected_descriptor),
                            &upstream_ops);
}

ZTEST(usb_identity, test_original_identity_is_installed)
{
    zassert_equal(codex_usb_vid(), 0x303A);
    zassert_equal(codex_usb_pid(), 0x8360);
    zassert_equal(strcmp(codex_usb_manufacturer(), "Work Louder"), 0);
    zassert_equal(strcmp(codex_usb_product(), "Codex Micro"), 0);
    zassert_equal(codex_usb_bcd_device(), 0x0100);
    zassert_equal(CONFIG_USB_HID_DEVICE_COUNT, 1);
    zassert_equal(codex_usb_report_descriptor_size(), 275U);
    zassert_equal(codex_usb_registered_report_descriptor(), codex_usb_report_descriptor);
    zassert_equal(codex_test_registered_descriptor(), codex_usb_report_descriptor);
    zassert_equal(codex_test_registered_descriptor_size(), 275U);
    zassert_equal(codex_test_registered_device(), fake_hid_device);
    zassert_ok(codex_usb_transport_init());
}

ZTEST(usb_identity, test_enumerated_device_descriptor_has_locked_release)
{
    static const uint8_t expected_manufacturer_utf16[] = {
        'W', 0, 'o', 0, 'r', 0, 'k', 0, ' ', 0, 'L', 0,
        'o', 0, 'u', 0, 'd', 0, 'e', 0, 'r', 0,
    };
    static const uint8_t expected_product_utf16[] = {
        'C', 0, 'o', 0, 'd', 0, 'e', 0, 'x', 0, ' ', 0,
        'M', 0, 'i', 0, 'c', 0, 'r', 0, 'o', 0,
    };
    struct usb_device_descriptor *descriptor =
        (struct usb_device_descriptor *)usb_get_device_descriptor();

    zassert_not_null(descriptor);
    zassert_equal(sys_le16_to_cpu(descriptor->idVendor), 0x303A);
    zassert_equal(sys_le16_to_cpu(descriptor->idProduct), 0x8360);
    zassert_equal(sys_le16_to_cpu(descriptor->bcdDevice), 0x0100);
    zassert_mem_equal(codex_test_manufacturer_utf16(), expected_manufacturer_utf16,
                      sizeof(expected_manufacturer_utf16));
    zassert_mem_equal(codex_test_product_utf16(), expected_product_utf16,
                      sizeof(expected_product_utf16));
}

ZTEST(usb_identity, test_vendor_send_keeps_report_id_outside_payload)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];
    uint8_t original[CODEX_VENDOR_PAYLOAD_SIZE];

    for (size_t index = 0U; index < sizeof(payload); index++) {
        payload[index] = (uint8_t)(index + 1U);
    }
    memcpy(original, payload, sizeof(original));

    zassert_ok(codex_usb_send_vendor(payload));
    zassert_equal(codex_test_last_write_size(), CODEX_INTERNAL_REPORT_SIZE);
    zassert_equal(codex_test_last_write()[0], CODEX_VENDOR_REPORT_ID);
    zassert_mem_equal(&codex_test_last_write()[1], original, sizeof(original));
    zassert_mem_equal(payload, original, sizeof(original));
}

ZTEST(usb_identity, test_vendor_output_is_normalized_before_delivery)
{
    uint8_t wire_report[CODEX_INTERNAL_REPORT_SIZE] = {
        [0] = CODEX_VENDOR_REPORT_ID,
    };
    struct usb_setup_packet setup = {
        .wValue = HID_REPORT_TYPE_OUTPUT | CODEX_VENDOR_REPORT_ID,
    };
    int32_t len = sizeof(wire_report);
    uint8_t *data = wire_report;

    for (size_t index = 1U; index < sizeof(wire_report); index++) {
        wire_report[index] = (uint8_t)(0x80U + index);
    }

    zassert_not_null(codex_test_registered_ops());
    zassert_not_null(codex_test_registered_ops()->set_report);
    zassert_ok(codex_test_registered_ops()->set_report(fake_hid_device, &setup, &len, &data));
    zassert_equal(codex_test_received_count(), 1U);
    zassert_mem_equal(codex_test_received_payload(), &wire_report[1],
                      CODEX_VENDOR_PAYLOAD_SIZE);
    zassert_equal(codex_test_bad_report_count(), 0U);
}

ZTEST(usb_identity, test_invalid_vendor_output_is_rejected)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE] = {0U};

    codex_usb_output_received(CODEX_VENDOR_REPORT_ID, payload,
                              CODEX_VENDOR_PAYLOAD_SIZE - 1U);
    codex_usb_output_received(CODEX_VENDOR_REPORT_ID + 1U, payload,
                              CODEX_VENDOR_PAYLOAD_SIZE);

    zassert_equal(codex_test_received_count(), 0U);
    zassert_equal(codex_test_bad_report_count(), 2U);
}

ZTEST(usb_identity, test_non_vendor_output_still_reaches_zmk_handler)
{
    uint8_t led_report[] = {0x01, 0x02};
    struct usb_setup_packet setup = {
        .wValue = HID_REPORT_TYPE_OUTPUT | 0x01U,
    };
    int32_t len = sizeof(led_report);
    uint8_t *data = led_report;

    zassert_equal(codex_test_registered_ops()->set_report(fake_hid_device, &setup, &len, &data),
                  -ENOTSUP);
    zassert_equal(codex_test_upstream_set_report_count(), 1U);
}

ZTEST_SUITE(usb_identity, NULL, NULL, usb_identity_before, NULL, NULL);
