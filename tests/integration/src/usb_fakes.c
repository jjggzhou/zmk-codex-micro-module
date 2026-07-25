#include "usb_fakes.h"

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_ch9.h>

#include <codex/descriptor.h>
#include <codex/usb.h>

static const struct device *registered_device;
static const uint8_t *registered_descriptor;
static size_t registered_descriptor_size;
static const struct hid_ops *registered_ops;
static uint8_t last_write[CODEX_INTERNAL_REPORT_SIZE];
static size_t last_write_size;
static uint8_t received_payload[CODEX_VENDOR_PAYLOAD_SIZE];
static size_t received_count;
static size_t bad_report_count;
static size_t upstream_set_report_count;

struct test_usb_string_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bString[22];
} __packed;

struct test_usb_string_description {
    struct usb_device_descriptor device;
    struct usb_cfg_descriptor configuration;
    struct usb_string_descriptor lang;
    struct test_usb_string_descriptor manufacturer;
    struct test_usb_string_descriptor product;
    struct usb_desc_header terminator;
} __packed;

static struct test_usb_string_description descriptor_block = {
    .device = {
        .bLength = sizeof(struct usb_device_descriptor),
        .bDescriptorType = USB_DESC_DEVICE,
        .idVendor = sys_cpu_to_le16(0x2FE3),
        .idProduct = sys_cpu_to_le16(0x0100),
        .bcdDevice = sys_cpu_to_le16(0x0305),
    },
    .configuration = {
        .bLength = sizeof(struct usb_cfg_descriptor),
        .bDescriptorType = USB_DESC_CONFIGURATION,
    },
    .lang = {
        .bLength = sizeof(struct usb_string_descriptor),
        .bDescriptorType = USB_DESC_STRING,
        .bString = sys_cpu_to_le16(0x0409),
    },
    .manufacturer = {
        .bLength = 24U,
        .bDescriptorType = USB_DESC_STRING,
    },
    .product = {
        .bLength = 24U,
        .bDescriptorType = USB_DESC_STRING,
    },
    .terminator = {
        .bLength = 0U,
        .bDescriptorType = 0U,
    },
};

void codex_test_usb_fakes_reset(void)
{
    registered_device = NULL;
    registered_descriptor = NULL;
    registered_descriptor_size = 0U;
    registered_ops = NULL;
    memset(last_write, 0, sizeof(last_write));
    last_write_size = 0U;
    memset(received_payload, 0, sizeof(received_payload));
    received_count = 0U;
    bad_report_count = 0U;
    upstream_set_report_count = 0U;
    memset(descriptor_block.manufacturer.bString, 0xA5,
           sizeof(descriptor_block.manufacturer.bString));
    memset(descriptor_block.product.bString, 0x5A, sizeof(descriptor_block.product.bString));
    descriptor_block.device.idVendor = sys_cpu_to_le16(0x2FE3);
    descriptor_block.device.idProduct = sys_cpu_to_le16(0x0100);
    descriptor_block.device.bcdDevice = sys_cpu_to_le16(0x0305);
}

void usb_hid_register_device(const struct device *dev, const uint8_t *desc, size_t size,
                             const struct hid_ops *ops)
{
    registered_device = dev;
    registered_descriptor = desc;
    registered_descriptor_size = size;
    registered_ops = ops;
}

uint8_t *usb_get_device_descriptor(void)
{
    return (uint8_t *)&descriptor_block;
}

int hid_int_ep_write(const struct device *dev, const uint8_t *data, uint32_t data_len,
                     uint32_t *bytes_ret)
{
    if (data_len > sizeof(last_write)) {
        return -1;
    }

    memcpy(last_write, data, data_len);
    last_write_size = data_len;
    if (bytes_ret != NULL) {
        *bytes_ret = data_len;
    }
    return 0;
}

void codex_usb_vendor_payload_received(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    memcpy(received_payload, payload, sizeof(received_payload));
    received_count++;
}

void codex_usb_bad_report_received(uint8_t report_id, size_t len)
{
    (void)report_id;
    (void)len;
    bad_report_count++;
}

const struct device *codex_test_registered_device(void) { return registered_device; }

const uint8_t *codex_test_registered_descriptor(void) { return registered_descriptor; }

size_t codex_test_registered_descriptor_size(void) { return registered_descriptor_size; }

const struct hid_ops *codex_test_registered_ops(void) { return registered_ops; }

const uint8_t *codex_test_last_write(void) { return last_write; }

size_t codex_test_last_write_size(void) { return last_write_size; }

size_t codex_test_received_count(void) { return received_count; }

const uint8_t *codex_test_received_payload(void) { return received_payload; }

size_t codex_test_bad_report_count(void) { return bad_report_count; }

size_t codex_test_upstream_set_report_count(void) { return upstream_set_report_count; }

void codex_test_upstream_set_report_called(void) { upstream_set_report_count++; }

const uint8_t *codex_test_manufacturer_utf16(void)
{
    return descriptor_block.manufacturer.bString;
}

const uint8_t *codex_test_product_utf16(void) { return descriptor_block.product.bString; }
