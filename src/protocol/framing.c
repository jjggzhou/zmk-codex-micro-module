#include <codex/framing.h>

#include "json_value.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#define CHANNEL_CANARY_BEFORE UINT32_C(0x434F4445)
#define CHANNEL_CANARY_AFTER UINT32_C(0x584A534E)

struct channel_state {
    uint32_t canary_before;
    uint8_t buffer[CODEX_JSON_MAX_SIZE];
    uint32_t canary_after;
    size_t len;
    bool number_waiting_delimiter;
    bool pending_protocol_cr;
    bool pending_cr_has_delivery;
    size_t pending_delivery_len;
};

struct transport_state {
    struct channel_state channels[2];
    uint32_t generation;
    bool generation_valid;
};

static struct transport_state states[CODEX_TRANSPORT_COUNT];

void codex_framing_reset_transport(enum codex_transport transport)
{
    if ((unsigned int)transport >= CODEX_TRANSPORT_COUNT) {
        return;
    }
    memset(&states[transport], 0, sizeof(states[transport]));
}

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
    state->pending_protocol_cr = false;
    state->pending_cr_has_delivery = false;
    state->pending_delivery_len = 0U;
}

static void reset_transport(struct transport_state *state)
{
    reset_channel(&state->channels[0]);
    reset_channel(&state->channels[1]);
}

static void initialize_transport(struct transport_state *state)
{
    for (size_t i = 0U; i < 2U; i++) {
        state->channels[i].canary_before = CHANNEL_CANARY_BEFORE;
        state->channels[i].canary_after = CHANNEL_CANARY_AFTER;
    }
    reset_transport(state);
}

#if defined(CONFIG_ZTEST)
bool codex_framing_test_canaries_intact(void)
{
    for (size_t transport = 0U; transport < CODEX_TRANSPORT_COUNT; transport++) {
        if (!states[transport].generation_valid) {
            continue;
        }
        for (size_t channel = 0U; channel < 2U; channel++) {
            if (states[transport].channels[channel].canary_before != CHANNEL_CANARY_BEFORE ||
                states[transport].channels[channel].canary_after != CHANNEL_CANARY_AFTER) {
                return false;
            }
        }
    }
    return true;
}
#endif

static bool padding_is_zero(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE], size_t len)
{
    for (size_t i = 2U + len; i < CODEX_VENDOR_PAYLOAD_SIZE; i++) {
        if (payload[i] != 0U) {
            return false;
        }
    }
    return true;
}

enum trailing_kind {
    TRAILING_JSON_WHITESPACE,
    TRAILING_PROTOCOL_TERMINATOR,
    TRAILING_PENDING_CR,
    TRAILING_INVALID,
};

struct trailing_analysis {
    enum trailing_kind kind;
    size_t delivery_len;
};

static struct trailing_analysis analyze_trailing(const struct channel_state *state,
                                                 size_t value_end)
{
    size_t cursor = value_end;

    while (cursor < state->len &&
           (state->buffer[cursor] == ' ' || state->buffer[cursor] == '\t')) {
        cursor++;
    }
    if (cursor == state->len) {
        return (struct trailing_analysis){TRAILING_JSON_WHITESPACE, state->len};
    }
    if (state->buffer[cursor] == '\n' && cursor + 1U == state->len) {
        return (struct trailing_analysis){TRAILING_PROTOCOL_TERMINATOR, cursor};
    }
    if (state->buffer[cursor] == '\r') {
        if (cursor + 1U == state->len) {
            return (struct trailing_analysis){TRAILING_PENDING_CR, cursor};
        }
        if (cursor + 2U == state->len && state->buffer[cursor + 1U] == '\n') {
            return (struct trailing_analysis){TRAILING_PROTOCOL_TERMINATOR, cursor};
        }
    }
    return (struct trailing_analysis){TRAILING_INVALID, 0U};
}

static int accept_generation(struct transport_state *state, uint32_t incoming)
{
    uint32_t difference;

    if (!state->generation_valid) {
        initialize_transport(state);
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
    struct trailing_analysis trailing = analyze_trailing(state, scan->value_end);
    bool has_json_whitespace = trailing.delivery_len > scan->value_end;

    if (trailing.kind == TRAILING_INVALID) {
        reset_channel(state);
        return -EINVAL;
    }
    if (trailing.kind == TRAILING_PROTOCOL_TERMINATOR) {
        return emit_and_reset(state, transport, channel, trailing.delivery_len, on_json);
    }
    if (trailing.kind == TRAILING_PENDING_CR) {
        state->len = trailing.delivery_len;
        state->pending_protocol_cr = true;
        state->pending_cr_has_delivery = true;
        state->pending_delivery_len = trailing.delivery_len;
        return 0;
    }
    if (has_json_whitespace) {
        if (fragment_len == CODEX_FRAGMENT_MAX) {
            return 0;
        }
        return emit_and_reset(state, transport, channel, trailing.delivery_len, on_json);
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

    transport_state = &states[transport];
    err = accept_generation(transport_state, connection_generation);
    if (err != 0) {
        return err;
    }
    channel = (enum codex_channel)payload[0];
    index = channel_index(channel);
    if (index < 0) {
        return -EINVAL;
    }
    channel_state = &transport_state->channels[index];
    fragment_len = payload[1];
    if (fragment_len > CODEX_FRAGMENT_MAX || !padding_is_zero(payload, fragment_len)) {
        reset_channel(channel_state);
        return -EINVAL;
    }

    if (channel_state->pending_protocol_cr) {
        size_t delivery_len = channel_state->pending_delivery_len;
        bool has_delivery = channel_state->pending_cr_has_delivery;

        if (fragment_len == 1U && payload[2] == '\n') {
            if (has_delivery) {
                return emit_and_reset(channel_state, transport, channel, delivery_len,
                                      on_json);
            }
            reset_channel(channel_state);
            return 0;
        }
        reset_channel(channel_state);
        return -EINVAL;
    }

    if (channel_state->len == 0U && !channel_state->number_waiting_delimiter) {
        if (fragment_len == 1U && payload[2] == '\n') {
            return 0;
        }
        if (fragment_len == 2U && payload[2] == '\r' && payload[3] == '\n') {
            return 0;
        }
        if (fragment_len == 1U && payload[2] == '\r') {
            channel_state->pending_protocol_cr = true;
            channel_state->pending_cr_has_delivery = false;
            channel_state->pending_delivery_len = 0U;
            return 0;
        }
    }

    if (fragment_len == 0U) {
        struct trailing_analysis trailing;

        scan = codex_json_value_scan(channel_state->buffer, channel_state->len,
                                     CODEX_JSON_MAX_DEPTH);
        if (scan.result == CODEX_JSON_COMPLETE) {
            trailing = analyze_trailing(channel_state, scan.value_end);
            if (trailing.kind == TRAILING_JSON_WHITESPACE &&
                (trailing.delivery_len > scan.value_end ||
                 (scan.root_kind == CODEX_JSON_ROOT_NUMBER &&
                  channel_state->number_waiting_delimiter))) {
                return emit_and_reset(channel_state, transport, channel,
                                      trailing.delivery_len, on_json);
            }
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
        struct trailing_analysis trailing;
        const uint8_t *overflow = &payload[2U + copied];

        if (scan.result != CODEX_JSON_COMPLETE) {
            reset_channel(channel_state);
            return -EMSGSIZE;
        }
        trailing = analyze_trailing(channel_state, scan.value_end);
        if (trailing.kind == TRAILING_JSON_WHITESPACE) {
            if (leftover == 1U && overflow[0] == '\r') {
                channel_state->pending_protocol_cr = true;
                channel_state->pending_cr_has_delivery = true;
                channel_state->pending_delivery_len = trailing.delivery_len;
                return 0;
            }
            if (leftover == 1U && overflow[0] == '\n') {
                return emit_and_reset(channel_state, transport, channel,
                                      trailing.delivery_len, on_json);
            }
            if (leftover == 2U && overflow[0] == '\r' && overflow[1] == '\n') {
                return emit_and_reset(channel_state, transport, channel,
                                      trailing.delivery_len, on_json);
            }
        } else if (trailing.kind == TRAILING_PENDING_CR &&
                   leftover == 1U && overflow[0] == '\n') {
            return emit_and_reset(channel_state, transport, channel,
                                  trailing.delivery_len, on_json);
        }
        reset_channel(channel_state);
        return -EMSGSIZE;
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
