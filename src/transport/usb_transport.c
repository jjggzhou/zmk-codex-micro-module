#include <codex/usb.h>

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/toolchain.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

#include <zmk/hid.h>
#include <zmk/usb.h>

#define HID_REPORT_TYPE_MASK UINT16_C(0xFF00)
#define HID_REPORT_ID_MASK UINT16_C(0x00FF)
#define HID_REPORT_TYPE_OUTPUT UINT16_C(0x0200)

#if defined(CONFIG_USB_DEVICE_STACK)
BUILD_ASSERT(CONFIG_USB_HID_DEVICE_COUNT == 1,
             "Codex Micro requires exactly one legacy USB HID interface");
#endif
BUILD_ASSERT(CONFIG_HID_INTERRUPT_EP_MPS == CODEX_INTERNAL_REPORT_SIZE,
             "Codex Micro HID IN endpoint must carry one 64-byte vendor report");
BUILD_ASSERT(CONFIG_USB_HID_REPORTS == CODEX_VENDOR_REPORT_ID,
             "Codex Micro idle report table must cover Report ID 6");
BUILD_ASSERT(!IS_ENABLED(CONFIG_ENABLE_HID_INT_OUT_EP),
             "Codex Micro uses control SET_REPORT and forbids an interrupt OUT endpoint");
BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_HID_REPORT_TYPE_HKRO),
             "Codex Micro golden descriptor requires ZMK HKRO reports");
BUILD_ASSERT(!IS_ENABLED(CONFIG_ZMK_HID_REPORT_TYPE_NKRO),
             "Codex Micro golden descriptor forbids ZMK NKRO reports");
BUILD_ASSERT(CONFIG_ZMK_HID_KEYBOARD_REPORT_SIZE == 6,
             "Codex Micro golden descriptor requires six keyboard usages");
BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_HID_CONSUMER_REPORT_USAGES_FULL),
             "Codex Micro golden descriptor requires full consumer usages");
BUILD_ASSERT(!IS_ENABLED(CONFIG_ZMK_HID_CONSUMER_REPORT_USAGES_BASIC),
             "Codex Micro golden descriptor forbids basic consumer usages");
BUILD_ASSERT(CONFIG_ZMK_HID_CONSUMER_REPORT_SIZE == 1,
             "Codex Micro golden descriptor requires one consumer usage");
BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_HID_INDICATORS),
             "Codex Micro golden descriptor requires the LED output report");
BUILD_ASSERT(!IS_ENABLED(CONFIG_ZMK_USB_BOOT),
             "Codex Micro exact composite descriptor does not expose boot protocol");
BUILD_ASSERT(sizeof(struct zmk_hid_keyboard_report) == 9U,
             "ZMK keyboard runtime report does not match the golden descriptor");
BUILD_ASSERT(sizeof(struct zmk_hid_consumer_report) == 3U,
             "ZMK consumer runtime report does not match the golden descriptor");
#if IS_ENABLED(CONFIG_ZMK_POINTING)
BUILD_ASSERT(sizeof(struct zmk_hid_mouse_report_body) == 9U,
             "ZMK mouse body is the expected BLE ABI before USB conversion");
#endif

static const struct device *codex_hid_device;
static const struct hid_ops *zmk_hid_ops;
static struct hid_ops codex_hid_ops;
static const uint8_t *registered_report_descriptor;
static K_SEM_DEFINE(codex_hid_in_sem, 1, 1);

extern void __real_usb_hid_register_device(const struct device *dev, const uint8_t *desc,
                                           size_t size, const struct hid_ops *ops);
extern int __real_hid_int_ep_write(const struct device *dev, const uint8_t *data,
                                   uint32_t data_len, uint32_t *bytes_ret);

__weak int
codex_usb_vendor_payload_received(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    (void)payload;
    return 0;
}

__weak void codex_usb_bad_report_received(uint8_t report_id, size_t len)
{
    (void)report_id;
    (void)len;
}

int codex_usb_output_received(uint8_t report_id, const uint8_t *data, size_t len)
{
    if (report_id == CODEX_VENDOR_REPORT_ID && data != NULL &&
        len == CODEX_VENDOR_PAYLOAD_SIZE) {
        return codex_usb_vendor_payload_received(data);
    }

    codex_usb_bad_report_received(report_id, len);
    return -EINVAL;
}

static int codex_set_report(const struct device *dev, struct usb_setup_packet *setup,
                            int32_t *len, uint8_t **data)
{
    uint16_t report_type = setup->wValue & HID_REPORT_TYPE_MASK;
    uint8_t report_id = (uint8_t)(setup->wValue & HID_REPORT_ID_MASK);

    if (report_type == HID_REPORT_TYPE_OUTPUT && report_id == CODEX_VENDOR_REPORT_ID) {
        if (*len == CODEX_INTERNAL_REPORT_SIZE && (*data)[0] == CODEX_VENDOR_REPORT_ID) {
            return codex_usb_output_received(report_id, &(*data)[1], CODEX_VENDOR_PAYLOAD_SIZE);
        }
        if (*len == CODEX_VENDOR_PAYLOAD_SIZE) {
            return codex_usb_output_received(report_id, *data, CODEX_VENDOR_PAYLOAD_SIZE);
        }

        codex_usb_output_received(report_id, *data, (size_t)*len);
        return -EINVAL;
    }

    if (zmk_hid_ops != NULL && zmk_hid_ops->set_report != NULL) {
        return zmk_hid_ops->set_report(dev, setup, len, data);
    }

    return -ENOTSUP;
}

static void codex_int_in_ready(const struct device *dev)
{
    k_sem_give(&codex_hid_in_sem);

    if (zmk_hid_ops != NULL && zmk_hid_ops->int_in_ready != NULL) {
        zmk_hid_ops->int_in_ready(dev);
    }
}

int __wrap_hid_int_ep_write(const struct device *dev, const uint8_t *data, uint32_t data_len,
                            uint32_t *bytes_ret)
{
    int err = k_sem_take(&codex_hid_in_sem, K_MSEC(30));

    if (err != 0) {
        return -EAGAIN;
    }

    err = __real_hid_int_ep_write(dev, data, data_len, bytes_ret);
    if (err != 0) {
        k_sem_give(&codex_hid_in_sem);
    }

    return err;
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
    k_sem_reset(&codex_hid_in_sem);
    k_sem_give(&codex_hid_in_sem);
    codex_hid_ops.int_in_ready = codex_int_in_ready;
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

#if IS_ENABLED(CONFIG_ZMK_POINTING)
static int8_t codex_mouse_axis_to_usb(int16_t value)
{
    if (value > INT8_MAX) {
        return INT8_MAX;
    }
    if (value < -INT8_MAX) {
        return -INT8_MAX;
    }
    return (int8_t)value;
}

int __wrap_zmk_usb_hid_send_mouse_report(void)
{
    const struct zmk_hid_mouse_report *report = zmk_hid_get_mouse_report();
    const uint8_t wire_report[6] = {
        ZMK_HID_REPORT_ID_MOUSE,
        report->body.buttons & BIT_MASK(ZMK_HID_MOUSE_NUM_BUTTONS),
        (uint8_t)codex_mouse_axis_to_usb(report->body.d_x),
        (uint8_t)codex_mouse_axis_to_usb(report->body.d_y),
        (uint8_t)codex_mouse_axis_to_usb(report->body.d_scroll_y),
        (uint8_t)codex_mouse_axis_to_usb(report->body.d_scroll_x),
    };

    switch (zmk_usb_get_status()) {
    case USB_DC_SUSPEND:
        return usb_wakeup_request();
    case USB_DC_ERROR:
    case USB_DC_RESET:
    case USB_DC_DISCONNECTED:
    case USB_DC_UNKNOWN:
        return -ENODEV;
    default:
        if (codex_hid_device == NULL) {
            return -ENODEV;
        }
        return hid_int_ep_write(codex_hid_device, wire_report, sizeof(wire_report), NULL);
    }
}
#endif
