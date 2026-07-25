#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>

#include <codex/ble_hids.h>

static uint8_t vendor_feature[CODEX_VENDOR_PAYLOAD_SIZE];
K_MUTEX_DEFINE(vendor_feature_lock);

int codex_ble_vendor_write_validate(const uint8_t *payload, size_t len, size_t offset,
                                    uint8_t write_flags)
{
    if ((write_flags & BT_GATT_WRITE_FLAG_PREPARE) != 0U) {
        return -ENOTSUP;
    }
    if (offset != 0U) {
        return -EFAULT;
    }
    if (payload == NULL || len != CODEX_VENDOR_PAYLOAD_SIZE) {
        return -EMSGSIZE;
    }
    return 0;
}

int codex_ble_vendor_notify_preflight(bool connected, bool subscribed, uint16_t att_mtu,
                                      const uint8_t *payload)
{
    if (payload == NULL) {
        return -EINVAL;
    }
    if (!connected) {
        return -ENOTCONN;
    }
    if (!subscribed) {
        return -EACCES;
    }
    if (att_mtu < CODEX_VENDOR_PAYLOAD_SIZE + 3U) {
        return -EMSGSIZE;
    }
    return 0;
}

int codex_ble_vendor_feature_get(uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    if (payload == NULL) {
        return -EINVAL;
    }
    k_mutex_lock(&vendor_feature_lock, K_FOREVER);
    memcpy(payload, vendor_feature, sizeof(vendor_feature));
    k_mutex_unlock(&vendor_feature_lock);
    return 0;
}

int codex_ble_vendor_feature_set(const uint8_t *payload, size_t len, size_t offset,
                                 uint8_t write_flags)
{
    int err = codex_ble_vendor_write_validate(payload, len, offset, write_flags);
    if (err != 0) {
        return err;
    }
    k_mutex_lock(&vendor_feature_lock, K_FOREVER);
    memcpy(vendor_feature, payload, sizeof(vendor_feature));
    k_mutex_unlock(&vendor_feature_lock);
    return (int)len;
}
