#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/ztest.h>

#include <codex/ble_hids.h>
#include <codex/descriptor.h>

enum { HID_INPUT = 0x80, HID_OUTPUT = 0x90, HID_FEATURE = 0xb0 };

struct report_shape {
    uint8_t id;
    uint8_t type;
    uint16_t bits;
};

static size_t item_size(uint8_t prefix)
{
    static const uint8_t sizes[] = {0, 1, 2, 4};
    return sizes[prefix & 3U];
}

static uint32_t item_value(const uint8_t *bytes, size_t len)
{
    uint32_t value = 0;
    for (size_t i = 0; i < len; i++) {
        value |= (uint32_t)bytes[i] << (8U * i);
    }
    return value;
}

static size_t parse_shapes(const uint8_t *descriptor, size_t descriptor_size,
                           struct report_shape *shapes, size_t capacity)
{
    uint8_t id = 0;
    uint16_t size = 0;
    uint16_t count = 0;
    size_t used = 0;

    for (size_t i = 0; i < descriptor_size;) {
        uint8_t prefix = descriptor[i++];
        size_t len = item_size(prefix);
        zassert_true(prefix != 0xfe && i + len <= descriptor_size);
        uint32_t value = item_value(&descriptor[i], len);

        if (prefix == 0x85) {
            id = value;
        } else if (prefix == 0x75) {
            size = value;
        } else if (prefix == 0x95) {
            count = value;
        } else if ((prefix & 0xfc) == HID_INPUT || (prefix & 0xfc) == HID_OUTPUT ||
                   (prefix & 0xfc) == HID_FEATURE) {
            uint8_t type = prefix & 0xfc;
            size_t j;
            for (j = 0; j < used; j++) {
                if (shapes[j].id == id && shapes[j].type == type) {
                    shapes[j].bits += size * count;
                    break;
                }
            }
            if (j == used) {
                zassert_true(used < capacity);
                shapes[used++] = (struct report_shape){id, type, size * count};
            }
        }
        i += len;
    }
    return used;
}

static uint16_t bits_for(const struct report_shape *shapes, size_t used, uint8_t id, uint8_t type)
{
    for (size_t i = 0; i < used; i++) {
        if (shapes[i].id == id && shapes[i].type == type) {
            return shapes[i].bits;
        }
    }
    return 0;
}

ZTEST(codex_ble_map, test_every_report_matches_the_production_gatt_contract)
{
    struct report_shape shapes[8] = {0};
    size_t used = parse_shapes(codex_ble_report_map, codex_ble_report_map_size(),
                               shapes, ARRAY_SIZE(shapes));

    zassert_equal(codex_ble_report_map_size(), 240);
    zassert_equal(used, 7, "map contains an orphan or missing report type");
    zassert_equal(bits_for(shapes, used, 1, HID_INPUT), 8U * 8U);
    zassert_equal(bits_for(shapes, used, 1, HID_OUTPUT), 1U * 8U);
    zassert_equal(bits_for(shapes, used, 2, HID_INPUT), 2U * 8U);
    zassert_equal(bits_for(shapes, used, 3, HID_INPUT), 9U * 8U);
    zassert_equal(bits_for(shapes, used, 6, HID_INPUT), 63U * 8U);
    zassert_equal(bits_for(shapes, used, 6, HID_OUTPUT), 63U * 8U);
    zassert_equal(bits_for(shapes, used, 6, HID_FEATURE), 63U * 8U);
}

ZTEST(codex_ble_map, test_mouse_payload_is_usb_five_bytes_but_ble_runtime_nine_bytes)
{
    struct report_shape usb[8] = {0};
    struct report_shape ble[8] = {0};
    size_t usb_used = parse_shapes(codex_usb_report_descriptor,
                                   codex_usb_report_descriptor_size(), usb, ARRAY_SIZE(usb));
    size_t ble_used = parse_shapes(codex_ble_report_map, codex_ble_report_map_size(),
                                   ble, ARRAY_SIZE(ble));

    zassert_equal(bits_for(usb, usb_used, 3, HID_INPUT), 5U * 8U);
    zassert_equal(bits_for(ble, ble_used, 3, HID_INPUT), 9U * 8U);
}

ZTEST(codex_ble_map, test_vendor_feature_has_exact_stable_write_semantics)
{
    uint8_t written[CODEX_VENDOR_PAYLOAD_SIZE];
    uint8_t readback[CODEX_VENDOR_PAYLOAD_SIZE] = {0};

    for (size_t i = 0; i < sizeof(written); i++) {
        written[i] = (uint8_t)(0xa5U ^ i);
    }
    zassert_equal(codex_ble_vendor_feature_set(written, sizeof(written), 0, 0),
                  sizeof(written));
    memset(written, 0, sizeof(written));
    zassert_ok(codex_ble_vendor_feature_get(readback));
    for (size_t i = 0; i < sizeof(readback); i++) {
        zassert_equal(readback[i], (uint8_t)(0xa5U ^ i));
    }

    zassert_equal(codex_ble_vendor_write_validate(readback, sizeof(readback) - 1U, 0, 0),
                  -EMSGSIZE);
    zassert_equal(codex_ble_vendor_write_validate(readback, sizeof(readback), 1, 0), -EFAULT);
    zassert_equal(codex_ble_vendor_write_validate(readback, sizeof(readback), 0,
                                                   BT_GATT_WRITE_FLAG_PREPARE),
                  -ENOTSUP);
    zassert_equal(codex_ble_vendor_write_validate(NULL, sizeof(readback), 0, 0), -EMSGSIZE);
}

ZTEST(codex_ble_map, test_notify_preflight_requires_connection_subscription_and_66_byte_mtu)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE] = {0};

    zassert_equal(codex_ble_vendor_notify_preflight(false, true, 66, payload), -ENOTCONN);
    zassert_equal(codex_ble_vendor_notify_preflight(true, false, 66, payload), -EACCES);
    zassert_equal(codex_ble_vendor_notify_preflight(true, true, 65, payload), -EMSGSIZE);
    zassert_equal(codex_ble_vendor_notify_preflight(true, true, 66, NULL), -EINVAL);
    zassert_ok(codex_ble_vendor_notify_preflight(true, true, 66, payload));
}

ZTEST_SUITE(codex_ble_map, NULL, NULL, NULL, NULL, NULL);
