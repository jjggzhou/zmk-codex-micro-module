#include "ble_queue.h"

#include <errno.h>
#include <string.h>

int codex_ble_owned_item_capture(struct codex_ble_owned_item *item, void *connection,
                                 uint8_t profile_index, uint32_t generation,
                                 const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
                                 codex_ble_ref_fn ref)
{
    if (item == NULL || connection == NULL || payload == NULL || ref == NULL) {
        return -EINVAL;
    }
    item->connection = ref(connection);
    if (item->connection == NULL) {
        return -ENOTCONN;
    }
    item->token.profile_index = profile_index;
    item->token.connection_generation = generation;
    memcpy(item->payload, payload, sizeof(item->payload));
    return 0;
}

void codex_ble_owned_item_release(struct codex_ble_owned_item *item, codex_ble_unref_fn unref)
{
    if (item != NULL && item->connection != NULL) {
        unref(item->connection);
        item->connection = NULL;
    }
}

bool codex_ble_owned_item_dispatch(struct codex_ble_owned_item *item, void *active_connection,
                                   uint8_t active_profile, uint32_t current_generation,
                                   codex_ble_emit_fn emit, void *context,
                                   codex_ble_unref_fn unref)
{
    bool current = item != NULL && item->connection == active_connection &&
                   item->token.profile_index == active_profile &&
                   item->token.connection_generation == current_generation;

    if (current && emit != NULL) {
        emit(item->payload, &item->token, context);
    }
    codex_ble_owned_item_release(item, unref);
    return current;
}
