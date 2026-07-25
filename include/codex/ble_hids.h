#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <codex/descriptor.h>

enum codex_ble_report_type {
    CODEX_BLE_REPORT_INPUT = 0x01,
    CODEX_BLE_REPORT_OUTPUT = 0x02,
    CODEX_BLE_REPORT_FEATURE = 0x03,
};

int codex_ble_vendor_notify(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE]);

struct codex_ble_source_token {
    uint8_t profile_index;
    uint32_t connection_generation;
};

/* Task 7 connects these worker-context seams to the endpoint router. */
void codex_ble_vendor_output_received(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE]);
void codex_ble_vendor_output_received_with_token(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
    const struct codex_ble_source_token *token);
void codex_ble_hids_purge_queues(void);

/* Stable Feature report state used by the encrypted GATT read/write callbacks. */
int codex_ble_vendor_feature_get(uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE]);
int codex_ble_vendor_feature_set(const uint8_t *payload, size_t len, size_t offset,
                                 uint8_t write_flags);
int codex_ble_vendor_write_validate(const uint8_t *payload, size_t len, size_t offset,
                                    uint8_t write_flags);
int codex_ble_vendor_notify_preflight(bool connected, bool subscribed, uint16_t att_mtu,
                                      const uint8_t *payload);

/* Artifact/probe metadata. These describe the production static GATT table. */
struct codex_ble_gatt_contract {
    uint16_t service_uuid;
    uint8_t report_id;
    uint8_t report_type;
    uint8_t payload_size;
    uint8_t properties;
    uint8_t permissions;
    uint8_t has_ccc;
};

extern const struct codex_ble_gatt_contract codex_ble_gatt_contract[];
extern const size_t codex_ble_gatt_contract_count;
