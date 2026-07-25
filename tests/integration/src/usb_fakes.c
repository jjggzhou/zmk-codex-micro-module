#include "usb_fakes.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_ch9.h>

#include <codex/descriptor.h>
#include <codex/usb.h>
#include <zmk/hid.h>

static const struct device *registered_device;
static const uint8_t *registered_descriptor;
static size_t registered_descriptor_size;
static const struct hid_ops *registered_ops;
static uint8_t last_write[CODEX_INTERNAL_REPORT_SIZE];
static size_t last_write_size;
static size_t write_attempt_count;
static bool write_in_flight;
static uint8_t received_payload[CODEX_VENDOR_PAYLOAD_SIZE];
static size_t received_count;
static size_t bad_report_count;
static size_t upstream_set_report_count;
static size_t upstream_in_ready_count;
static struct zmk_hid_mouse_report mouse_report = {
    .report_id = ZMK_HID_REPORT_ID_MOUSE,
};
static enum usb_dc_status_code usb_status = USB_DC_CONFIGURED;
static size_t wakeup_count;

#if defined(CONFIG_HID_INTERRUPT_EP_MPS)
#define CODEX_TEST_HID_INTERRUPT_EP_MPS CONFIG_HID_INTERRUPT_EP_MPS
#else
#define CODEX_TEST_HID_INTERRUPT_EP_MPS 16
#endif

struct test_usb_string_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bString[22];
} __packed;

struct test_usb_hid_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bReportDescriptorType;
    uint16_t wDescriptorLength;
} __packed;

struct test_usb_serial_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bString[16];
} __packed;

struct test_usb_descriptor_block {
    struct usb_device_descriptor device;
    struct usb_cfg_descriptor configuration;
    struct usb_if_descriptor interface;
    struct test_usb_hid_descriptor hid;
    struct usb_ep_descriptor interrupt_in;
    struct usb_string_descriptor lang;
    struct test_usb_string_descriptor manufacturer;
    struct test_usb_string_descriptor product;
    struct test_usb_serial_descriptor serial;
    struct usb_desc_header terminator;
    uint8_t canary[4];
} __packed;

static struct test_usb_descriptor_block descriptor_block = {
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
        .bNumInterfaces = 1U,
    },
    .interface = {
        .bLength = sizeof(struct usb_if_descriptor),
        .bDescriptorType = USB_DESC_INTERFACE,
        .bNumEndpoints = 1U,
        .bInterfaceClass = USB_BCC_HID,
    },
    .hid = {
        .bLength = sizeof(struct test_usb_hid_descriptor),
        .bDescriptorType = USB_DESC_HID,
        .bcdHID = sys_cpu_to_le16(USB_HID_VERSION),
        .bNumDescriptors = 1U,
        .bReportDescriptorType = USB_DESC_HID_REPORT,
        .wDescriptorLength = sys_cpu_to_le16(275U),
    },
    .interrupt_in = {
        .bLength = sizeof(struct usb_ep_descriptor),
        .bDescriptorType = USB_DESC_ENDPOINT,
        .bEndpointAddress = USB_EP_DIR_IN | 1U,
        .bmAttributes = USB_DC_EP_INTERRUPT,
        .wMaxPacketSize = sys_cpu_to_le16(CODEX_INTERNAL_REPORT_SIZE),
        .bInterval = 1U,
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
    .serial = {
        .bLength = sizeof(struct test_usb_serial_descriptor),
        .bDescriptorType = USB_DESC_STRING,
        .bString = {
            '0', 0, '1', 0, '2', 0, '3', 0,
            '4', 0, '5', 0, '6', 0, '7', 0,
        },
    },
    .terminator = {
        .bLength = 0U,
        .bDescriptorType = 0U,
    },
    .canary = {0xC1, 0xC2, 0xC3, 0xC4},
};

void codex_test_usb_fakes_reset(void)
{
    registered_device = NULL;
    registered_descriptor = NULL;
    registered_descriptor_size = 0U;
    registered_ops = NULL;
    memset(last_write, 0, sizeof(last_write));
    last_write_size = 0U;
    write_attempt_count = 0U;
    write_in_flight = false;
    memset(received_payload, 0, sizeof(received_payload));
    received_count = 0U;
    bad_report_count = 0U;
    upstream_set_report_count = 0U;
    upstream_in_ready_count = 0U;
    memset(&mouse_report.body, 0, sizeof(mouse_report.body));
    usb_status = USB_DC_CONFIGURED;
    wakeup_count = 0U;
    memset(descriptor_block.manufacturer.bString, 0xA5,
           sizeof(descriptor_block.manufacturer.bString));
    memset(descriptor_block.product.bString, 0x5A, sizeof(descriptor_block.product.bString));
    descriptor_block.interrupt_in.bLength = sizeof(struct usb_ep_descriptor);
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

uint8_t *codex_usb_descriptor_start(void) { return (uint8_t *)&descriptor_block; }

uint8_t *codex_usb_descriptor_end(void) { return (uint8_t *)&descriptor_block.canary; }

int hid_int_ep_write(const struct device *dev, const uint8_t *data, uint32_t data_len,
                     uint32_t *bytes_ret)
{
    (void)dev;
    write_attempt_count++;

    if (data_len > CODEX_TEST_HID_INTERRUPT_EP_MPS) {
        return -EMSGSIZE;
    }
    if (write_in_flight) {
        return -EAGAIN;
    }

    memcpy(last_write, data, data_len);
    last_write_size = data_len;
    write_in_flight = true;
    if (bytes_ret != NULL) {
        *bytes_ret = data_len;
    }
    return 0;
}

struct zmk_hid_mouse_report *zmk_hid_get_mouse_report(void) { return &mouse_report; }

enum usb_dc_status_code zmk_usb_get_status(void) { return usb_status; }

int usb_wakeup_request(void)
{
    wakeup_count++;
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

size_t codex_test_write_attempt_count(void) { return write_attempt_count; }

bool codex_test_write_in_flight(void) { return write_in_flight; }

void codex_test_complete_write(void)
{
    if (!write_in_flight) {
        return;
    }

    write_in_flight = false;
    if (registered_ops != NULL && registered_ops->int_in_ready != NULL) {
        registered_ops->int_in_ready(registered_device);
    }
}

void codex_test_set_mouse_report(struct zmk_hid_mouse_report report) { mouse_report = report; }

void codex_test_set_usb_status(enum usb_dc_status_code status) { usb_status = status; }

size_t codex_test_wakeup_count(void) { return wakeup_count; }

size_t codex_test_received_count(void) { return received_count; }

const uint8_t *codex_test_received_payload(void) { return received_payload; }

size_t codex_test_bad_report_count(void) { return bad_report_count; }

size_t codex_test_upstream_set_report_count(void) { return upstream_set_report_count; }

void codex_test_upstream_set_report_called(void) { upstream_set_report_count++; }

void codex_test_upstream_in_ready_called(void) { upstream_in_ready_count++; }

size_t codex_test_upstream_in_ready_count(void) { return upstream_in_ready_count; }

uint8_t *codex_test_descriptor_block(void) { return (uint8_t *)&descriptor_block; }

size_t codex_test_descriptor_block_size(void)
{
    return (size_t)((uint8_t *)&descriptor_block.canary - (uint8_t *)&descriptor_block);
}

void codex_test_corrupt_endpoint_length(uint8_t length)
{
    descriptor_block.interrupt_in.bLength = length;
}

uint16_t codex_test_descriptor_vid(void)
{
    return sys_le16_to_cpu(descriptor_block.device.idVendor);
}

const uint8_t *codex_test_manufacturer_utf16(void)
{
    return descriptor_block.manufacturer.bString;
}

const uint8_t *codex_test_product_utf16(void) { return descriptor_block.product.bString; }

const uint8_t *codex_test_serial_utf16(void) { return descriptor_block.serial.bString; }

const uint8_t *codex_test_descriptor_canary(void) { return descriptor_block.canary; }
