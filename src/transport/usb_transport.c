#include <codex/usb.h>

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/toolchain.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

#define HID_REPORT_TYPE_MASK UINT16_C(0xFF00)
#define HID_REPORT_ID_MASK UINT16_C(0x00FF)
#define HID_REPORT_TYPE_OUTPUT UINT16_C(0x0200)

#if defined(CONFIG_USB_DEVICE_STACK)
BUILD_ASSERT(CONFIG_USB_HID_DEVICE_COUNT == 1,
             "Codex Micro requires exactly one legacy USB HID interface");
#endif

static const struct device *codex_hid_device;
static const struct hid_ops *zmk_hid_ops;
static struct hid_ops codex_hid_ops;
static const uint8_t *registered_report_descriptor;

extern void __real_usb_hid_register_device(const struct device *dev, const uint8_t *desc,
                                           size_t size, const struct hid_ops *ops);

__weak void
codex_usb_vendor_payload_received(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    (void)payload;
}

__weak void codex_usb_bad_report_received(uint8_t report_id, size_t len)
{
    (void)report_id;
    (void)len;
}

void codex_usb_output_received(uint8_t report_id, const uint8_t *data, size_t len)
{
    if (report_id == CODEX_VENDOR_REPORT_ID && data != NULL &&
        len == CODEX_VENDOR_PAYLOAD_SIZE) {
        codex_usb_vendor_payload_received(data);
        return;
    }

    codex_usb_bad_report_received(report_id, len);
}

static int codex_set_report(const struct device *dev, struct usb_setup_packet *setup,
                            int32_t *len, uint8_t **data)
{
    uint16_t report_type = setup->wValue & HID_REPORT_TYPE_MASK;
    uint8_t report_id = (uint8_t)(setup->wValue & HID_REPORT_ID_MASK);

    if (report_type == HID_REPORT_TYPE_OUTPUT && report_id == CODEX_VENDOR_REPORT_ID) {
        if (*len == CODEX_INTERNAL_REPORT_SIZE && (*data)[0] == CODEX_VENDOR_REPORT_ID) {
            codex_usb_output_received(report_id, &(*data)[1], CODEX_VENDOR_PAYLOAD_SIZE);
            return 0;
        }
        if (*len == CODEX_VENDOR_PAYLOAD_SIZE) {
            codex_usb_output_received(report_id, *data, CODEX_VENDOR_PAYLOAD_SIZE);
            return 0;
        }

        codex_usb_output_received(report_id, *data, (size_t)*len);
        return -EINVAL;
    }

    if (zmk_hid_ops != NULL && zmk_hid_ops->set_report != NULL) {
        return zmk_hid_ops->set_report(dev, setup, len, data);
    }

    return -ENOTSUP;
}

void __wrap_usb_hid_register_device(const struct device *dev, const uint8_t *desc, size_t size,
                                    const struct hid_ops *ops)
{
    (void)desc;
    (void)size;

    codex_hid_device = dev;
    zmk_hid_ops = ops;
    if (ops == NULL) {
        memset(&codex_hid_ops, 0, sizeof(codex_hid_ops));
    } else {
        codex_hid_ops = *ops;
    }
    codex_hid_ops.set_report = codex_set_report;
    registered_report_descriptor = codex_usb_report_descriptor;

    __real_usb_hid_register_device(dev, codex_usb_report_descriptor,
                                   codex_usb_report_descriptor_size(), &codex_hid_ops);
}

const uint8_t *codex_usb_registered_report_descriptor(void)
{
    return registered_report_descriptor;
}

int codex_usb_transport_init(void)
{
    return codex_hid_device == NULL ? -ENODEV : 0;
}

int codex_usb_send_vendor(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    uint8_t wire_report[CODEX_INTERNAL_REPORT_SIZE];

    if (payload == NULL) {
        return -EINVAL;
    }
    if (codex_hid_device == NULL) {
        return -ENODEV;
    }

    wire_report[0] = CODEX_VENDOR_REPORT_ID;
    memcpy(&wire_report[1], payload, CODEX_VENDOR_PAYLOAD_SIZE);

    return hid_int_ep_write(codex_hid_device, wire_report, sizeof(wire_report), NULL);
}
