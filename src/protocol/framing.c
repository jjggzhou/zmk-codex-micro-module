#include <codex/framing.h>

#include "json_value.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

struct channel_state {
    uint8_t buffer[CODEX_JSON_MAX_SIZE];
    size_t len;
    bool pending_cr;
};

struct transport_state {
    struct channel_state channels[2];
    uint32_t generation;
    bool generation_valid;
};

static struct transport_state states[CODEX_TRANSPORT_COUNT];

static int channel_index(enum codex_channel channel)
{
    if (channel == CODEX_CHANNEL_DEBUG) {
        return 0;
    }
    if (channel == CODEX_CHANNEL_RPC) {
        return 1;
    }
    return -1;
}

static void reset_transport(struct transport_state *state)
{
    state->channels[0].len = 0U;
    state->channels[0].pending_cr = false;
    state->channels[1].len = 0U;
    state->channels[1].pending_cr = false;
}

static bool padding_is_zero(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE], size_t len)
{
    for (size_t i = 2U + len; i < CODEX_VENDOR_PAYLOAD_SIZE; i++) {
        if (payload[i] != 0U) {
            return false;
        }
    }
    return true;
}

int codex_framing_ingest(enum codex_transport transport,
                         const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
                         uint32_t connection_generation,
                         codex_json_cb_t on_json)
{
    struct transport_state *transport_state;
    struct channel_state *channel_state;
    enum codex_channel channel;
    enum codex_json_result result;
    size_t fragment_len;
    size_t fragment_offset = 0U;
    size_t append_len;
    bool has_terminator = false;
    int index;

    if ((unsigned int)transport >= CODEX_TRANSPORT_COUNT || payload == NULL ||
        on_json == NULL) {
        return -EINVAL;
    }

    transport_state = &states[transport];
    if (!transport_state->generation_valid ||
        transport_state->generation != connection_generation) {
        reset_transport(transport_state);
        transport_state->generation = connection_generation;
        transport_state->generation_valid = true;
    }

    channel = (enum codex_channel)payload[0];
    index = channel_index(channel);
    if (index < 0) {
        reset_transport(transport_state);
        return -EINVAL;
    }
    channel_state = &transport_state->channels[index];
    fragment_len = payload[1];
    if (fragment_len > CODEX_FRAGMENT_MAX || !padding_is_zero(payload, fragment_len)) {
        channel_state->len = 0U;
        channel_state->pending_cr = false;
        return -EINVAL;
    }

    append_len = fragment_len;
    if (channel_state->pending_cr) {
        if (append_len == 0U || payload[2] != '\n') {
            channel_state->len = 0U;
            channel_state->pending_cr = false;
            return -EINVAL;
        }
        channel_state->pending_cr = false;
        has_terminator = true;
        fragment_offset = 1U;
        append_len--;
        if (append_len != 0U) {
            channel_state->len = 0U;
            return -EINVAL;
        }
    } else if (append_len > 0U && payload[2U + append_len - 1U] == '\n') {
        has_terminator = true;
        append_len--;
        if (append_len > 0U && payload[2U + append_len - 1U] == '\r') {
            append_len--;
        }
    } else if (append_len > 0U && payload[2U + append_len - 1U] == '\r') {
        channel_state->pending_cr = true;
        append_len--;
    }

    if (append_len > CODEX_JSON_MAX_SIZE - channel_state->len) {
        channel_state->len = 0U;
        channel_state->pending_cr = false;
        return -EMSGSIZE;
    }

    if (append_len > 0U) {
        memcpy(&channel_state->buffer[channel_state->len],
               &payload[2U + fragment_offset], append_len);
        channel_state->len += append_len;
    }
    result = codex_json_value_validate(channel_state->buffer, channel_state->len,
                                       CODEX_JSON_MAX_DEPTH);
    if (result == CODEX_JSON_INVALID ||
        (result == CODEX_JSON_INCOMPLETE && (fragment_len == 0U || has_terminator))) {
        channel_state->len = 0U;
        channel_state->pending_cr = false;
        return -EINVAL;
    }
    if (result == CODEX_JSON_COMPLETE && !channel_state->pending_cr) {
        on_json(transport, channel, channel_state->buffer, channel_state->len);
        channel_state->len = 0U;
    }
    return 0;
}

int codex_framing_encode(enum codex_channel channel,
                         const uint8_t *json, size_t len,
                         codex_emit_report_t emit, void *ctx)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];
    size_t offset = 0U;
    int err;

    if (channel_index(channel) < 0 || json == NULL || emit == NULL || len == 0U) {
        return -EINVAL;
    }
    if (len > CODEX_JSON_MAX_SIZE) {
        return -EMSGSIZE;
    }
    if (codex_json_value_validate(json, len, CODEX_JSON_MAX_DEPTH) !=
        CODEX_JSON_COMPLETE) {
        return -EINVAL;
    }

    while (len - offset + 2U > CODEX_FRAGMENT_MAX) {
        size_t remaining = len - offset;
        size_t chunk = remaining > CODEX_FRAGMENT_MAX ? CODEX_FRAGMENT_MAX
                                                      : remaining - 1U;

        memset(payload, 0, sizeof(payload));
        payload[0] = (uint8_t)channel;
        payload[1] = (uint8_t)chunk;
        memcpy(&payload[2], &json[offset], chunk);
        err = emit(payload, ctx);
        if (err != 0) {
            return err;
        }
        offset += chunk;
    }

    memset(payload, 0, sizeof(payload));
    payload[0] = (uint8_t)channel;
    payload[1] = (uint8_t)(len - offset + 2U);
    memcpy(&payload[2], &json[offset], len - offset);
    payload[2U + len - offset] = '\r';
    payload[3U + len - offset] = '\n';
    return emit(payload, ctx);
}
