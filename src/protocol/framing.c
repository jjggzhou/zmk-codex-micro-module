#include <codex/framing.h>

#include "json_value.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

struct channel_state {
    uint8_t buffer[CODEX_JSON_MAX_SIZE];
    size_t len;
    bool number_waiting_delimiter;
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

static void reset_channel(struct channel_state *state)
{
    state->len = 0U;
    state->number_waiting_delimiter = false;
}

static void reset_transport(struct transport_state *state)
{
    reset_channel(&state->channels[0]);
    reset_channel(&state->channels[1]);
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

static bool is_lf_or_crlf(const uint8_t *data, size_t len)
{
    return (len == 1U && data[0] == '\n') ||
           (len == 2U && data[0] == '\r' && data[1] == '\n');
}

static int accept_generation(struct transport_state *state, uint32_t incoming)
{
    uint32_t difference;

    if (!state->generation_valid) {
        state->generation = incoming;
        state->generation_valid = true;
        return 0;
    }

    difference = incoming - state->generation;
    if (difference == 0U) {
        return 0;
    }
    if (difference == UINT32_C(0x80000000) || (int32_t)difference < 0) {
        return -ESTALE;
    }

    reset_transport(state);
    state->generation = incoming;
    return 0;
}

static int emit_and_reset(struct channel_state *state, enum codex_transport transport,
                          enum codex_channel channel, size_t value_len,
                          codex_json_cb_t on_json)
{
    on_json(transport, channel, state->buffer, value_len);
    reset_channel(state);
    return 0;
}

static int handle_complete_value(struct channel_state *state,
                                 enum codex_transport transport,
                                 enum codex_channel channel,
                                 const struct codex_json_scan *scan,
                                 size_t fragment_len,
                                 codex_json_cb_t on_json)
{
    const uint8_t *trailing = &state->buffer[scan->value_end];
    size_t trailing_len = state->len - scan->value_end;

    if (trailing_len > 0U) {
        if (is_lf_or_crlf(trailing, trailing_len)) {
            return emit_and_reset(state, transport, channel, scan->value_end, on_json);
        }
        if (trailing_len == 1U && trailing[0] == '\r') {
            return 0;
        }
        reset_channel(state);
        return -EINVAL;
    }

    if (scan->root_kind != CODEX_JSON_ROOT_NUMBER) {
        return emit_and_reset(state, transport, channel, scan->value_end, on_json);
    }
    if (!state->number_waiting_delimiter && fragment_len < CODEX_FRAGMENT_MAX) {
        return emit_and_reset(state, transport, channel, scan->value_end, on_json);
    }

    state->number_waiting_delimiter = true;
    return 0;
}

int codex_framing_ingest(enum codex_transport transport,
                         const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
                         uint32_t connection_generation,
                         codex_json_cb_t on_json)
{
    struct transport_state *transport_state;
    struct channel_state *channel_state;
    struct codex_json_scan scan;
    enum codex_channel channel;
    size_t fragment_len;
    size_t available;
    size_t copied;
    size_t leftover;
    int index;
    int err;

    if ((unsigned int)transport >= CODEX_TRANSPORT_COUNT || payload == NULL ||
        on_json == NULL) {
        return -EINVAL;
    }

    channel = (enum codex_channel)payload[0];
    index = channel_index(channel);
    if (index < 0) {
        return -EINVAL;
    }

    transport_state = &states[transport];
    err = accept_generation(transport_state, connection_generation);
    if (err != 0) {
        return err;
    }
    channel_state = &transport_state->channels[index];
    fragment_len = payload[1];
    if (fragment_len > CODEX_FRAGMENT_MAX || !padding_is_zero(payload, fragment_len)) {
        reset_channel(channel_state);
        return -EINVAL;
    }

    if (fragment_len == 0U) {
        scan = codex_json_value_scan(channel_state->buffer, channel_state->len,
                                     CODEX_JSON_MAX_DEPTH);
        if (scan.result == CODEX_JSON_COMPLETE &&
            scan.value_end == channel_state->len &&
            scan.root_kind == CODEX_JSON_ROOT_NUMBER &&
            channel_state->number_waiting_delimiter) {
            return emit_and_reset(channel_state, transport, channel, scan.value_end,
                                  on_json);
        }
        reset_channel(channel_state);
        return -EINVAL;
    }

    available = CODEX_JSON_MAX_SIZE - channel_state->len;
    copied = fragment_len < available ? fragment_len : available;
    if (copied > 0U) {
        memcpy(&channel_state->buffer[channel_state->len], &payload[2], copied);
        channel_state->len += copied;
    }
    leftover = fragment_len - copied;
    scan = codex_json_value_scan(channel_state->buffer, channel_state->len,
                                 CODEX_JSON_MAX_DEPTH);

    if (leftover > 0U) {
        const uint8_t *trailing_in_buffer;
        size_t trailing_in_buffer_len;
        uint8_t terminator[2];
        size_t terminator_len = 0U;

        if (scan.result != CODEX_JSON_COMPLETE) {
            reset_channel(channel_state);
            return -EMSGSIZE;
        }
        trailing_in_buffer = &channel_state->buffer[scan.value_end];
        trailing_in_buffer_len = channel_state->len - scan.value_end;
        if (trailing_in_buffer_len > sizeof(terminator) ||
            leftover > sizeof(terminator) - trailing_in_buffer_len) {
            reset_channel(channel_state);
            return -EMSGSIZE;
        }
        memcpy(terminator, trailing_in_buffer, trailing_in_buffer_len);
        terminator_len += trailing_in_buffer_len;
        memcpy(&terminator[terminator_len], &payload[2U + copied], leftover);
        terminator_len += leftover;
        if (!is_lf_or_crlf(terminator, terminator_len)) {
            reset_channel(channel_state);
            return -EMSGSIZE;
        }
        return emit_and_reset(channel_state, transport, channel, scan.value_end,
                              on_json);
    }

    if (scan.result == CODEX_JSON_INVALID) {
        reset_channel(channel_state);
        return -EINVAL;
    }
    if (scan.root_kind == CODEX_JSON_ROOT_NUMBER &&
        scan.result == CODEX_JSON_INCOMPLETE) {
        channel_state->number_waiting_delimiter = true;
    }
    if (scan.result == CODEX_JSON_INCOMPLETE) {
        return 0;
    }

    return handle_complete_value(channel_state, transport, channel, &scan,
                                 fragment_len, on_json);
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
