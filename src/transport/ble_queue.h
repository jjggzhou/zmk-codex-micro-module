#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <codex/ble_hids.h>

struct codex_ble_owned_item {
    void *connection;
    struct codex_ble_source_token token;
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];
};

typedef void *(*codex_ble_ref_fn)(void *connection);
typedef void (*codex_ble_unref_fn)(void *connection);
typedef void (*codex_ble_emit_fn)(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
                                  const struct codex_ble_source_token *token, void *context);

int codex_ble_owned_item_capture(struct codex_ble_owned_item *item, void *connection,
                                 uint8_t profile_index, uint32_t generation,
                                 const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
                                 codex_ble_ref_fn ref);
void codex_ble_owned_item_release(struct codex_ble_owned_item *item, codex_ble_unref_fn unref);
bool codex_ble_owned_item_dispatch(struct codex_ble_owned_item *item, void *active_connection,
                                   uint8_t active_profile, uint32_t current_generation,
                                   codex_ble_emit_fn emit, void *context,
                                   codex_ble_unref_fn unref);
