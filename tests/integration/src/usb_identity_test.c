#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/ztest.h>

#include <codex/descriptor.h>
#include <codex/usb.h>
#include <zmk/hid.h>

#include "usb_fakes.h"

#define HID_REPORT_TYPE_OUTPUT UINT16_C(0x0200)

extern uint8_t *usb_get_device_descriptor(void);
extern bool codex_usb_apply_identity(uint8_t *descriptor_block, size_t descriptor_block_size);
extern void codex_test_upstream_set_report_called(void);
extern void codex_test_upstream_in_ready_called(void);

static const struct device *const fake_hid_device =
    (const struct device *)(uintptr_t)UINT32_C(0x1234);

#define COMPETING_WRITER_STACK_SIZE 1024

enum competing_write_kind {
    COMPETING_WRITE_KEYBOARD,
    COMPETING_WRITE_VENDOR,
};

struct competing_write_context {
    enum competing_write_kind kind;
    const uint8_t *data;
    size_t size;
    int result;
};

K_THREAD_STACK_DEFINE(competing_writer_stack, COMPETING_WRITER_STACK_SIZE);
static struct k_thread competing_writer_thread;
static K_SEM_DEFINE(competing_writer_started, 0, 1);
static K_SEM_DEFINE(competing_writer_done, 0, 1);
static struct competing_write_context competing_writer_context;

static void competing_writer(void *context_pointer, void *unused1, void *unused2)
{
    struct competing_write_context *context = context_pointer;

    (void)unused1;
    (void)unused2;
    k_sem_give(&competing_writer_started);

    if (context->kind == COMPETING_WRITE_KEYBOARD) {
        context->result =
            hid_int_ep_write(fake_hid_device, context->data, context->size, NULL);
    } else {
        context->result = codex_usb_send_vendor(context->data);
    }

    k_sem_give(&competing_writer_done);
}

static void start_competing_writer(enum competing_write_kind kind, const uint8_t *data,
                                   size_t size)
{
    k_sem_reset(&competing_writer_started);
    k_sem_reset(&competing_writer_done);
    competing_writer_context.kind = kind;
    competing_writer_context.data = data;
    competing_writer_context.size = size;
    competing_writer_context.result = INT_MIN;

    k_thread_create(&competing_writer_thread, competing_writer_stack,
                    K_THREAD_STACK_SIZEOF(competing_writer_stack), competing_writer,
                    &competing_writer_context, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
    zassert_ok(k_sem_take(&competing_writer_started, K_MSEC(100)));
}

static void finish_competing_writer(void)
{
    zassert_ok(k_sem_take(&competing_writer_done, K_MSEC(100)));
    zassert_ok(k_thread_join(&competing_writer_thread, K_MSEC(100)));
}

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

static void upstream_in_ready(const struct device *dev)
{
    (void)dev;
    codex_test_upstream_in_ready_called();
}

static const struct hid_ops upstream_ops = {
    .int_in_ready = upstream_in_ready,
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

ZTEST(usb_identity, test_endpoint_and_zmk_runtime_abi_are_locked)
{
    zassert_equal(CONFIG_HID_INTERRUPT_EP_MPS, 64);
    zassert_equal(CONFIG_USB_HID_REPORTS, 6);
    zassert_false(IS_ENABLED(CONFIG_ENABLE_HID_INT_OUT_EP));
    zassert_true(IS_ENABLED(CONFIG_ZMK_HID_REPORT_TYPE_HKRO));
    zassert_false(IS_ENABLED(CONFIG_ZMK_HID_REPORT_TYPE_NKRO));
    zassert_equal(CONFIG_ZMK_HID_KEYBOARD_REPORT_SIZE, 6);
    zassert_true(IS_ENABLED(CONFIG_ZMK_HID_CONSUMER_REPORT_USAGES_FULL));
    zassert_false(IS_ENABLED(CONFIG_ZMK_HID_CONSUMER_REPORT_USAGES_BASIC));
    zassert_equal(CONFIG_ZMK_HID_CONSUMER_REPORT_SIZE, 1);
    zassert_true(IS_ENABLED(CONFIG_ZMK_HID_INDICATORS));
    zassert_equal(sizeof(struct zmk_hid_keyboard_report), 9U);
    zassert_equal(sizeof(struct zmk_hid_consumer_report), 3U);
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
    static const uint8_t expected_serial_utf16[] = {
        '0', 0, '1', 0, '2', 0, '3', 0,
        '4', 0, '5', 0, '6', 0, '7', 0,
    };
    static const uint8_t expected_canary[] = {0xC1, 0xC2, 0xC3, 0xC4};
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
    zassert_mem_equal(codex_test_serial_utf16(), expected_serial_utf16,
                      sizeof(expected_serial_utf16));
    zassert_mem_equal(codex_test_descriptor_canary(), expected_canary, sizeof(expected_canary));
}

ZTEST(usb_identity, test_identity_patch_rejects_descriptor_that_crosses_linker_end)
{
    static const uint8_t expected_canary[] = {0xC1, 0xC2, 0xC3, 0xC4};

    codex_test_corrupt_endpoint_length(UINT8_MAX);

    zassert_false(codex_usb_apply_identity(codex_test_descriptor_block(),
                                           codex_test_descriptor_block_size()));
    zassert_equal(codex_test_descriptor_vid(), 0x2FE3);
    zassert_mem_equal(codex_test_descriptor_canary(), expected_canary, sizeof(expected_canary));
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
    codex_test_complete_write();
}

ZTEST(usb_identity, test_vendor_then_keyboard_write_is_serialized)
{
    uint8_t vendor_payload[CODEX_VENDOR_PAYLOAD_SIZE] = {0xA6};
    uint8_t keyboard_report[sizeof(struct zmk_hid_keyboard_report)] = {
        [0] = ZMK_HID_REPORT_ID_KEYBOARD,
    };

    zassert_ok(codex_usb_send_vendor(vendor_payload));
    zassert_true(codex_test_write_in_flight());
    start_competing_writer(COMPETING_WRITE_KEYBOARD, keyboard_report, sizeof(keyboard_report));
    zassert_equal(k_sem_take(&competing_writer_done, K_MSEC(5)), -EAGAIN);
    zassert_equal(codex_test_write_attempt_count(), 1U);

    codex_test_complete_write();
    finish_competing_writer();
    zassert_ok(competing_writer_context.result);
    zassert_equal(codex_test_upstream_in_ready_count(), 1U);
    zassert_mem_equal(codex_test_last_write(), keyboard_report, sizeof(keyboard_report));
    codex_test_complete_write();
    zassert_equal(codex_test_upstream_in_ready_count(), 2U);
}

ZTEST(usb_identity, test_keyboard_then_vendor_write_is_serialized)
{
    uint8_t keyboard_report[sizeof(struct zmk_hid_keyboard_report)] = {
        [0] = ZMK_HID_REPORT_ID_KEYBOARD,
    };
    uint8_t vendor_payload[CODEX_VENDOR_PAYLOAD_SIZE] = {0xB6};

    zassert_ok(hid_int_ep_write(fake_hid_device, keyboard_report, sizeof(keyboard_report), NULL));
    zassert_true(codex_test_write_in_flight());
    start_competing_writer(COMPETING_WRITE_VENDOR, vendor_payload, sizeof(vendor_payload));
    zassert_equal(k_sem_take(&competing_writer_done, K_MSEC(5)), -EAGAIN);
    zassert_equal(codex_test_write_attempt_count(), 1U);

    codex_test_complete_write();
    finish_competing_writer();
    zassert_ok(competing_writer_context.result);
    zassert_equal(codex_test_upstream_in_ready_count(), 1U);
    zassert_equal(codex_test_last_write()[0], CODEX_VENDOR_REPORT_ID);
    zassert_mem_equal(&codex_test_last_write()[1], vendor_payload, sizeof(vendor_payload));
    codex_test_complete_write();
    zassert_equal(codex_test_upstream_in_ready_count(), 2U);
}

ZTEST(usb_identity, test_competing_write_times_out_without_completion)
{
    uint8_t vendor_payload[CODEX_VENDOR_PAYLOAD_SIZE] = {0xC6};
    uint8_t keyboard_report[sizeof(struct zmk_hid_keyboard_report)] = {
        [0] = ZMK_HID_REPORT_ID_KEYBOARD,
    };

    zassert_ok(codex_usb_send_vendor(vendor_payload));
    start_competing_writer(COMPETING_WRITE_KEYBOARD, keyboard_report, sizeof(keyboard_report));
    zassert_equal(k_sem_take(&competing_writer_done, K_MSEC(5)), -EAGAIN);
    finish_competing_writer();
    zassert_equal(competing_writer_context.result, -EAGAIN);
    zassert_equal(codex_test_write_attempt_count(), 1U);

    codex_test_complete_write();
    zassert_equal(codex_test_upstream_in_ready_count(), 1U);
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
