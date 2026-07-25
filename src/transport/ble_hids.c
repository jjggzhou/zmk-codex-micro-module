/*
 * Copyright (c) 2020 The ZMK Contributors
 * Copyright (c) 2026 Codex Micro compatibility contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * This translation unit is a narrow replacement wrapper around the pinned
 * ZMK v0.3 app/src/hog.c. CMake verifies both its Git commit and file hash,
 * excludes that source from the app target, then includes it below. Only the
 * static HIDS definition and report-map token are adapted; upstream keyboard,
 * consumer, mouse, LED, queue, and profile behavior remains source-identical.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/ble.h>
#include <zmk/hid.h>

#include <codex/ble_hids.h>

#include "ble_queue.h"

BUILD_ASSERT(CONFIG_BT_L2CAP_TX_MTU == 69, "Codex BLE ATT TX MTU contract changed");
BUILD_ASSERT(CONFIG_BT_BUF_ACL_RX_SIZE == 73, "Codex BLE ACL RX buffer contract changed");
BUILD_ASSERT(CONFIG_BT_BUF_ACL_TX_SIZE == 73, "Codex BLE ACL TX buffer contract changed");
BUILD_ASSERT(IS_ENABLED(CONFIG_BT_GATT_AUTO_UPDATE_MTU),
             "Codex BLE requires automatic ATT MTU exchange on connect");
BUILD_ASSERT(CONFIG_ZMK_HID_KEYBOARD_REPORT_SIZE == 6,
             "BLE map requires the ZMK v0.3 8-byte HKRO keyboard body");
BUILD_ASSERT(CONFIG_ZMK_HID_CONSUMER_REPORT_SIZE == 1,
             "BLE map requires one 16-bit consumer usage");
BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_HID_REPORT_TYPE_HKRO), "BLE map requires HKRO");
BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_HID_CONSUMER_REPORT_USAGES_FULL),
             "BLE map requires 16-bit consumer usages");
BUILD_ASSERT(IS_ENABLED(CONFIG_ZMK_POINTING), "BLE map includes the ZMK mouse report");
BUILD_ASSERT(!IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING),
             "BLE map has no mouse resolution feature report");
BUILD_ASSERT(sizeof(struct zmk_hid_keyboard_report_body) == 8,
             "keyboard GATT value/map mismatch");
BUILD_ASSERT(sizeof(struct zmk_hid_consumer_report_body) == 2,
             "consumer GATT value/map mismatch");
BUILD_ASSERT(sizeof(struct zmk_hid_mouse_report_body) == 9,
             "mouse GATT value/map mismatch");
BUILD_ASSERT(CODEX_VENDOR_PAYLOAD_SIZE + 3U <= CONFIG_BT_L2CAP_TX_MTU,
             "ATT MTU cannot carry the vendor notification");

struct codex_hids_report_ref {
    uint8_t id;
    uint8_t type;
} __packed;

static const struct codex_hids_report_ref vendor_input_ref = {
    .id = CODEX_VENDOR_REPORT_ID, .type = CODEX_BLE_REPORT_INPUT};
static const struct codex_hids_report_ref vendor_output_ref = {
    .id = CODEX_VENDOR_REPORT_ID, .type = CODEX_BLE_REPORT_OUTPUT};
static const struct codex_hids_report_ref vendor_feature_ref = {
    .id = CODEX_VENDOR_REPORT_ID, .type = CODEX_BLE_REPORT_FEATURE};

static uint8_t vendor_output_state[CODEX_VENDOR_PAYLOAD_SIZE];
K_MUTEX_DEFINE(vendor_output_lock);
static uint8_t vendor_input_state[CODEX_VENDOR_PAYLOAD_SIZE];
K_MUTEX_DEFINE(vendor_input_lock);

K_MSGQ_DEFINE(vendor_output_msgq, sizeof(struct codex_ble_owned_item), 4, 4);
K_MSGQ_DEFINE(vendor_notify_msgq, sizeof(struct codex_ble_owned_item), 4, 4);
static atomic_t connection_generation;

__weak void
codex_ble_vendor_output_received(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    ARG_UNUSED(payload);
}

__weak void codex_ble_vendor_output_received_with_token(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
    const struct codex_ble_source_token *token)
{
    ARG_UNUSED(token);
    codex_ble_vendor_output_received(payload);
}

static void *codex_bt_conn_ref(void *connection)
{
    return bt_conn_ref(connection);
}

static void codex_bt_conn_unref(void *connection)
{
    bt_conn_unref(connection);
}

static bool queue_item_is_current(const struct codex_ble_owned_item *item,
                                  const struct bt_gatt_attr *input_attr,
                                  bool require_subscription)
{
    struct bt_conn *active;
    int active_profile;
    bool current;

    if (item->token.connection_generation != (uint32_t)atomic_get(&connection_generation)) {
        return false;
    }
    active = zmk_ble_active_profile_conn();
    if (active == NULL) {
        return false;
    }
    active_profile = zmk_ble_profile_index(bt_conn_get_dst(active));
    current = active == item->connection && active_profile == item->token.profile_index;
    bt_conn_unref(active);
    if (!current) {
        return false;
    }
    if (require_subscription &&
        (!bt_gatt_is_subscribed(item->connection, input_attr, BT_GATT_CCC_NOTIFY) ||
         bt_gatt_get_mtu(item->connection) < CODEX_VENDOR_PAYLOAD_SIZE + 3U)) {
        return false;
    }
    return true;
}

static int queue_item_capture(struct codex_ble_owned_item *item, struct bt_conn *conn,
                              const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    int profile;

    if (conn == NULL) {
        return -ENOTCONN;
    }
    profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0 || profile > UINT8_MAX) {
        return -ENOTCONN;
    }
    return codex_ble_owned_item_capture(item, conn, (uint8_t)profile,
                                        (uint32_t)atomic_get(&connection_generation), payload,
                                        codex_bt_conn_ref);
}

static void queue_item_release(struct codex_ble_owned_item *item)
{
    codex_ble_owned_item_release(item, codex_bt_conn_unref);
}

static void vendor_output_emit(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
                               const struct codex_ble_source_token *token, void *context)
{
    ARG_UNUSED(context);
    codex_ble_vendor_output_received_with_token(payload, token);
}

static ssize_t read_codex_report_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                     void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
                             sizeof(struct codex_hids_report_ref));
}

static ssize_t read_codex_feature(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  void *buf, uint16_t len, uint16_t offset)
{
    uint8_t snapshot[CODEX_VENDOR_PAYLOAD_SIZE];
    int err = codex_ble_vendor_feature_get(snapshot);

    if (err != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, snapshot, sizeof(snapshot));
}

static ssize_t write_codex_feature(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    int err = codex_ble_vendor_feature_set(buf, len, offset, flags);
    if (err == -ENOTSUP) {
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    } else if (err == -EFAULT) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    } else if (err == -EMSGSIZE) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    return err;
}

static ssize_t read_codex_output(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 void *buf, uint16_t len, uint16_t offset)
{
    uint8_t snapshot[CODEX_VENDOR_PAYLOAD_SIZE];
    k_mutex_lock(&vendor_output_lock, K_FOREVER);
    memcpy(snapshot, vendor_output_state, sizeof(snapshot));
    k_mutex_unlock(&vendor_output_lock);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, snapshot, sizeof(snapshot));
}

static ssize_t read_codex_input(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset)
{
    uint8_t snapshot[CODEX_VENDOR_PAYLOAD_SIZE];
    k_mutex_lock(&vendor_input_lock, K_FOREVER);
    memcpy(snapshot, vendor_input_state, sizeof(snapshot));
    k_mutex_unlock(&vendor_input_lock);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, snapshot, sizeof(snapshot));
}

static void vendor_output_work_handler(struct k_work *work)
{
    struct codex_ble_owned_item item;

    ARG_UNUSED(work);
    while (k_msgq_get(&vendor_output_msgq, &item, K_NO_WAIT) == 0) {
        struct bt_conn *active = zmk_ble_active_profile_conn();
        uint8_t active_profile = UINT8_MAX;

        if (active != NULL) {
            int profile = zmk_ble_profile_index(bt_conn_get_dst(active));
            if (profile >= 0 && profile <= UINT8_MAX) {
                active_profile = (uint8_t)profile;
            }
        }
        codex_ble_owned_item_dispatch(&item, active, active_profile,
                                      (uint32_t)atomic_get(&connection_generation),
                                      vendor_output_emit, NULL, codex_bt_conn_unref);
        if (active != NULL) {
            bt_conn_unref(active);
        }
    }
}
K_WORK_DEFINE(vendor_output_work, vendor_output_work_handler);

static ssize_t write_codex_output(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    int err;
    struct codex_ble_owned_item item;

    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    err = codex_ble_vendor_write_validate(buf, len, offset, flags);
    if (err == -ENOTSUP) {
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    } else if (err == -EFAULT) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    } else if (err == -EMSGSIZE) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    err = queue_item_capture(&item, conn, buf);
    if (err != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    err = k_msgq_put(&vendor_output_msgq, &item, K_NO_WAIT);
    if (err != 0) {
        queue_item_release(&item);
        return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }
    k_mutex_lock(&vendor_output_lock, K_FOREVER);
    memcpy(vendor_output_state, buf, sizeof(vendor_output_state));
    k_mutex_unlock(&vendor_output_lock);
    k_work_submit(&vendor_output_work);
    return len;
}

#define CODEX_VENDOR_HIDS_ATTRIBUTES                                                        \
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,                                             \
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,                          \
                           BT_GATT_PERM_READ_ENCRYPT, read_codex_input, NULL, NULL),         \
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),              \
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT,                  \
                       read_codex_report_ref, NULL, (void *)&vendor_input_ref),              \
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,                                             \
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |                         \
                               BT_GATT_CHRC_WRITE_WITHOUT_RESP,                             \
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,           \
                           read_codex_output, write_codex_output, NULL),                     \
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT,                  \
                       read_codex_report_ref, NULL, (void *)&vendor_output_ref),             \
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,                                             \
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |                         \
                               BT_GATT_CHRC_WRITE_WITHOUT_RESP,                             \
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,           \
                           read_codex_feature, write_codex_feature, NULL),                   \
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT,                  \
                       read_codex_report_ref, NULL, (void *)&vendor_feature_ref)

/* Replace exactly the one service declaration in pinned app/src/hog.c. */
#undef BT_GATT_SERVICE_DEFINE
#define BT_GATT_SERVICE_DEFINE(_name, ...)                                                   \
    const struct bt_gatt_attr attr_##_name[] = {__VA_ARGS__, CODEX_VENDOR_HIDS_ATTRIBUTES};  \
    const STRUCT_SECTION_ITERABLE(bt_gatt_service_static, _name) = BT_GATT_SERVICE(attr_##_name)

/* The include is accepted only after CMake's pinned commit and SHA-256 gate. */
#define zmk_hid_report_desc codex_ble_report_map
#include CODEX_PINNED_ZMK_HOG_SOURCE
#undef zmk_hid_report_desc

/* With pointing + indicators and no smooth scrolling, vendor input declaration is attr 22. */
#define CODEX_VENDOR_INPUT_DECL_ATTR_INDEX 22U
BUILD_ASSERT(ARRAY_SIZE(attr_hog_svc) == 32U, "pinned HIDS attribute layout changed");

static void vendor_notify_work_handler(struct k_work *work)
{
    struct codex_ble_owned_item item;

    ARG_UNUSED(work);
    while (k_msgq_get(&vendor_notify_msgq, &item, K_NO_WAIT) == 0) {
        struct bt_gatt_notify_params params = {
            .attr = &hog_svc.attrs[CODEX_VENDOR_INPUT_DECL_ATTR_INDEX],
            .data = item.payload,
            .len = sizeof(item.payload),
        };

        if (!queue_item_is_current(&item, params.attr, true)) {
            queue_item_release(&item);
            continue;
        }
        k_mutex_lock(&vendor_input_lock, K_FOREVER);
        memcpy(vendor_input_state, item.payload, sizeof(vendor_input_state));
        k_mutex_unlock(&vendor_input_lock);
        /* Zephyr 3.5 copies params.data into a net_buf before returning. */
        int err = bt_gatt_notify_cb(item.connection, &params);
        if (err == -EPERM) {
            bt_conn_set_security(item.connection, BT_SECURITY_L2);
        }
        queue_item_release(&item);
    }
}
K_WORK_DEFINE(vendor_notify_work, vendor_notify_work_handler);

int codex_ble_vendor_notify(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    int err;
    struct bt_conn *conn;
    struct codex_ble_owned_item item;

    if (payload == NULL) {
        return -EINVAL;
    }
    conn = zmk_ble_active_profile_conn();
    if (conn == NULL) {
        return -ENOTCONN;
    }
    err = codex_ble_vendor_notify_preflight(
        true,
        bt_gatt_is_subscribed(conn, &hog_svc.attrs[CODEX_VENDOR_INPUT_DECL_ATTR_INDEX],
                              BT_GATT_CCC_NOTIFY),
        bt_gatt_get_mtu(conn), payload);
    if (err == 0) {
        err = queue_item_capture(&item, conn, payload);
    }
    bt_conn_unref(conn);
    if (err != 0) {
        return err;
    }
    err = k_msgq_put(&vendor_notify_msgq, &item, K_NO_WAIT);
    if (err != 0) {
        queue_item_release(&item);
        return err;
    }
    k_work_submit(&vendor_notify_work);
    return 0;
}

void codex_ble_hids_purge_queues(void)
{
    struct codex_ble_owned_item item;

    atomic_inc(&connection_generation);
    while (k_msgq_get(&vendor_notify_msgq, &item, K_NO_WAIT) == 0) {
        queue_item_release(&item);
    }
    while (k_msgq_get(&vendor_output_msgq, &item, K_NO_WAIT) == 0) {
        queue_item_release(&item);
    }
}

uint32_t codex_ble_hids_generation(void)
{
    return (uint32_t)atomic_get(&connection_generation);
}

bool codex_ble_hids_token_is_current(const struct codex_ble_source_token *token,
                                     uint8_t active_profile)
{
    return token != NULL && token->profile_index == active_profile &&
           token->connection_generation == codex_ble_hids_generation();
}

const struct codex_ble_gatt_contract codex_ble_gatt_contract[]
    __attribute__((used)) = {
    {0x1812, 0x01, CODEX_BLE_REPORT_INPUT, 8, 0x12, 0x04, 1},
    {0x1812, 0x01, CODEX_BLE_REPORT_OUTPUT, 1, 0x0e, 0x0c, 0},
    {0x1812, 0x02, CODEX_BLE_REPORT_INPUT, 2, 0x12, 0x04, 1},
    {0x1812, 0x03, CODEX_BLE_REPORT_INPUT, 9, 0x12, 0x04, 1},
    {0x1812, 0x06, CODEX_BLE_REPORT_INPUT, 63, 0x12, 0x04, 1},
    {0x1812, 0x06, CODEX_BLE_REPORT_OUTPUT, 63, 0x0e, 0x0c, 0},
    {0x1812, 0x06, CODEX_BLE_REPORT_FEATURE, 63, 0x0e, 0x0c, 0},
};
const size_t codex_ble_gatt_contract_count = ARRAY_SIZE(codex_ble_gatt_contract);
