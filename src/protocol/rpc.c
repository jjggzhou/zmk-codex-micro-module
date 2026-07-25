#include <codex/rpc.h>

#include "json_value.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/sys/util.h>

#define RPC_MAX_TOP_LEVEL_FIELDS 32U

struct span {
    const uint8_t *data;
    size_t len;
};

struct request {
    struct span method;
    struct span id;
    bool has_method;
    bool has_id;
    bool has_jsonrpc;
    bool compact_method;
};

struct writer {
    uint8_t data[CODEX_JSON_MAX_SIZE];
    size_t len;
    int error;
};

enum method_kind {
    METHOD_SYS_VERSION,
    METHOD_DEVICE_STATUS,
    METHOD_RGB_CONFIG,
    METHOD_AGENT_STATUS,
    METHOD_LIGHTS_PREVIEW,
    METHOD_ACTIVE_SCREEN,
    METHOD_HOME_ACCENT_COLOR,
    METHOD_FOCUSED_APP,
    METHOD_FORBIDDEN,
    METHOD_UNKNOWN,
};

struct method_entry {
    const char *name;
    enum method_kind kind;
};

static const struct method_entry supported_methods[] = {
    {"sys.version", METHOD_SYS_VERSION},
    {"device.status", METHOD_DEVICE_STATUS},
    {"v.oai.rgbcfg", METHOD_RGB_CONFIG},
    {"v.oai.thstatus", METHOD_AGENT_STATUS},
    {"lights.preview", METHOD_LIGHTS_PREVIEW},
    {"ui.active_screen", METHOD_ACTIVE_SCREEN},
    {"ui.home_accent_color", METHOD_HOME_ACCENT_COLOR},
    {"host.focused_app", METHOD_FOCUSED_APP},
};

static bool is_space(uint8_t byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

static void skip_space(const uint8_t *json, size_t len, size_t *pos)
{
    while (*pos < len && is_space(json[*pos])) {
        (*pos)++;
    }
}

static int hex_value(uint8_t byte)
{
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    return byte - 'A' + 10;
}

static uint16_t decode_hex4(const uint8_t *data)
{
    uint16_t value = 0U;

    for (size_t i = 0U; i < 4U; i++) {
        value = (uint16_t)((value << 4U) | (uint16_t)hex_value(data[i]));
    }
    return value;
}

static int next_string_codepoint(struct span string, size_t *pos, uint32_t *codepoint)
{
    uint8_t byte;

    if (*pos >= string.len - 1U) {
        return 0;
    }
    byte = string.data[(*pos)++];
    if (byte == '\\') {
        uint8_t escape = string.data[(*pos)++];

        switch (escape) {
        case '"': case '\\': case '/':
            *codepoint = escape;
            return 1;
        case 'b': *codepoint = '\b'; return 1;
        case 'f': *codepoint = '\f'; return 1;
        case 'n': *codepoint = '\n'; return 1;
        case 'r': *codepoint = '\r'; return 1;
        case 't': *codepoint = '\t'; return 1;
        default: {
            uint16_t first = decode_hex4(&string.data[*pos]);

            *pos += 4U;
            if (first >= 0xD800U && first <= 0xDBFFU) {
                uint16_t second;

                *pos += 2U;
                second = decode_hex4(&string.data[*pos]);
                *pos += 4U;
                *codepoint = UINT32_C(0x10000) +
                             (((uint32_t)first - UINT32_C(0xD800)) << 10U) +
                             ((uint32_t)second - UINT32_C(0xDC00));
            } else {
                *codepoint = first;
            }
            return 1;
        }
        }
    }
    if (byte < 0x80U) {
        *codepoint = byte;
        return 1;
    }

    size_t continuation_count = byte < 0xE0U ? 1U : byte < 0xF0U ? 2U : 3U;
    uint32_t value = byte & (0x7FU >> continuation_count);

    for (size_t i = 0U; i < continuation_count; i++) {
        value = (value << 6U) | (string.data[(*pos)++] & 0x3FU);
    }
    *codepoint = value;
    return 1;
}

static bool string_equals_literal(struct span string, const char *literal)
{
    size_t pos = 1U;
    size_t literal_pos = 0U;
    uint32_t codepoint;

    if (string.len < 2U || string.data[0] != '"' ||
        string.data[string.len - 1U] != '"') {
        return false;
    }
    while (next_string_codepoint(string, &pos, &codepoint) != 0) {
        if (codepoint > UINT8_MAX || literal[literal_pos] == '\0' ||
            codepoint != (uint8_t)literal[literal_pos]) {
            return false;
        }
        literal_pos++;
    }
    return literal[literal_pos] == '\0';
}

static bool strings_equal(struct span left, struct span right)
{
    size_t left_pos = 1U;
    size_t right_pos = 1U;

    for (;;) {
        uint32_t left_codepoint;
        uint32_t right_codepoint;
        int has_left = next_string_codepoint(left, &left_pos, &left_codepoint);
        int has_right = next_string_codepoint(right, &right_pos, &right_codepoint);

        if (has_left == 0 || has_right == 0) {
            return has_left == has_right;
        }
        if (left_codepoint != right_codepoint) {
            return false;
        }
    }
}

static size_t skip_string_token(const uint8_t *json, size_t pos)
{
    pos++;
    while (json[pos] != '"') {
        if (json[pos++] == '\\') {
            if (json[pos++] == 'u') {
                pos += 4U;
            }
        }
    }
    return pos + 1U;
}

static size_t skip_value(const uint8_t *json, size_t len, size_t pos)
{
    if (json[pos] == '"') {
        return skip_string_token(json, pos);
    }
    if (json[pos] == '{' || json[pos] == '[') {
        /* Mixed nested delimiters are valid, so scan with a small delimiter stack. */
        uint8_t stack[CODEX_JSON_MAX_DEPTH];
        size_t depth = 0U;

        do {
            if (json[pos] == '"') {
                pos = skip_string_token(json, pos);
                continue;
            }
            if (json[pos] == '{' || json[pos] == '[') {
                stack[depth++] = json[pos] == '{' ? '}' : ']';
            } else if (depth > 0U && json[pos] == stack[depth - 1U]) {
                depth--;
            }
            pos++;
        } while (depth > 0U);
        return pos;
    }
    while (pos < len && json[pos] != ',' && json[pos] != '}' && !is_space(json[pos])) {
        pos++;
    }
    return pos;
}

static bool span_is_integer_or_null(struct span value)
{
    if (value.len == 4U && memcmp(value.data, "null", 4U) == 0) {
        return true;
    }
    if (value.len == 0U || (value.data[0] != '-' &&
                            (value.data[0] < '0' || value.data[0] > '9'))) {
        return false;
    }
    for (size_t i = value.data[0] == '-' ? 1U : 0U; i < value.len; i++) {
        if (value.data[i] < '0' || value.data[i] > '9') {
            return false;
        }
    }
    return true;
}

static bool span_is_params(struct span value)
{
    return (value.len == 4U && memcmp(value.data, "null", 4U) == 0) ||
           (value.len > 0U && (value.data[0] == '{' || value.data[0] == '['));
}

static int parse_request(const uint8_t *json, size_t len, struct request *request)
{
    struct span keys[RPC_MAX_TOP_LEVEL_FIELDS];
    size_t key_count = 0U;
    size_t pos = 0U;
    bool has_params = false;

    if (codex_json_value_validate(json, len, CODEX_JSON_MAX_DEPTH) !=
        CODEX_JSON_COMPLETE) {
        return -EINVAL;
    }
    skip_space(json, len, &pos);
    if (pos >= len || json[pos++] != '{') {
        return -EINVAL;
    }
    skip_space(json, len, &pos);
    while (pos < len && json[pos] != '}') {
        size_t key_start = pos;
        size_t value_start;
        size_t value_end;
        struct span key;
        struct span value;

        if (json[pos] != '"' || key_count == ARRAY_SIZE(keys)) {
            return -EINVAL;
        }
        pos = skip_string_token(json, pos);
        key = (struct span){&json[key_start], pos - key_start};
        for (size_t i = 0U; i < key_count; i++) {
            if (strings_equal(key, keys[i])) {
                return -EINVAL;
            }
        }
        keys[key_count++] = key;
        skip_space(json, len, &pos);
        if (json[pos++] != ':') {
            return -EINVAL;
        }
        skip_space(json, len, &pos);
        value_start = pos;
        value_end = skip_value(json, len, pos);
        value = (struct span){&json[value_start], value_end - value_start};
        pos = value_end;

        if (string_equals_literal(key, "m") ||
            string_equals_literal(key, "method")) {
            if (request->has_method || value.len < 2U || value.data[0] != '"') {
                return -EINVAL;
            }
            request->has_method = true;
            request->compact_method = string_equals_literal(key, "m");
            request->method = value;
        } else if (string_equals_literal(key, "p") ||
                   string_equals_literal(key, "params")) {
            if (has_params || !span_is_params(value)) {
                return -EINVAL;
            }
            has_params = true;
        } else if (string_equals_literal(key, "id")) {
            if (request->has_id ||
                !((value.len >= 2U && value.data[0] == '"') ||
                  span_is_integer_or_null(value))) {
                return -EINVAL;
            }
            request->has_id = true;
            request->id = value;
        } else if (string_equals_literal(key, "jsonrpc")) {
            if (request->has_jsonrpc || !string_equals_literal(value, "2.0")) {
                return -EINVAL;
            }
            request->has_jsonrpc = true;
        }

        skip_space(json, len, &pos);
        if (json[pos] == ',') {
            pos++;
            skip_space(json, len, &pos);
        } else if (json[pos] != '}') {
            return -EINVAL;
        }
    }
    if (!request->has_method || (!request->compact_method && !request->has_jsonrpc)) {
        return -EINVAL;
    }
    return 0;
}

static void write_bytes(struct writer *writer, const void *data, size_t len)
{
    if (writer->error != 0) {
        return;
    }
    if (len > sizeof(writer->data) - writer->len) {
        writer->error = -EMSGSIZE;
        return;
    }
    memcpy(&writer->data[writer->len], data, len);
    writer->len += len;
}

static void write_literal(struct writer *writer, const char *literal)
{
    write_bytes(writer, literal, strlen(literal));
}

static void write_uint(struct writer *writer, uint8_t value)
{
    char digits[4];
    int len = snprintf(digits, sizeof(digits), "%u", value);

    write_bytes(writer, digits, (size_t)len);
}

static enum method_kind identify_method(struct span method, const char **canonical)
{
    for (size_t i = 0U; i < ARRAY_SIZE(supported_methods); i++) {
        if (string_equals_literal(method, supported_methods[i].name)) {
            *canonical = supported_methods[i].name;
            return supported_methods[i].kind;
        }
    }
    if (string_equals_literal(method, "sys.bootloader") ||
        string_equals_literal(method, "sys.selftest") ||
        string_equals_literal(method, "fs") ||
        string_equals_literal(method, "mp") ||
        string_equals_literal(method, "wlsdk")) {
        return METHOD_FORBIDDEN;
    }
    static const char *const prefixes[] = {"fs.", "mp.", "wlsdk."};

    for (size_t i = 0U; i < ARRAY_SIZE(prefixes); i++) {
        size_t prefix_len = strlen(prefixes[i]);
        size_t pos = 1U;

        for (size_t j = 0U; j < prefix_len; j++) {
            uint32_t codepoint;

            if (next_string_codepoint(method, &pos, &codepoint) == 0 ||
                codepoint != (uint8_t)prefixes[i][j]) {
                goto next_prefix;
            }
        }
        return METHOD_FORBIDDEN;
next_prefix:
        continue;
    }
    return METHOD_UNKNOWN;
}

static void write_id_and_close(struct writer *writer, struct span id,
                               const char *method)
{
    write_literal(writer, ",\"id\":");
    write_bytes(writer, id.data, id.len);
    if (method != NULL) {
        write_literal(writer, ",\"method\":\"");
        write_literal(writer, method);
        write_literal(writer, "\"");
    }
    write_literal(writer, "}");
}

static void write_error(struct writer *writer, int code, const char *message,
                        struct span id)
{
    char code_text[4];
    int code_len = snprintf(code_text, sizeof(code_text), "%d", code);

    write_literal(writer, "{\"error\":{\"code\":");
    write_bytes(writer, code_text, (size_t)code_len);
    write_literal(writer, ",\"message\":\"");
    write_literal(writer, message);
    write_literal(writer, "\"}");
    write_id_and_close(writer, id, NULL);
}

static void write_success(struct writer *writer, enum method_kind kind,
                          const char *method, struct span id)
{
    write_literal(writer, "{\"result\":");
    switch (kind) {
    case METHOD_SYS_VERSION:
        write_literal(writer, "{\"version\":\"");
        write_literal(writer, CODEX_FIRMWARE_VERSION);
        write_literal(writer, "\"}");
        break;
    case METHOD_DEVICE_STATUS: {
        struct codex_device_status status = codex_device_status_snapshot();

        write_literal(writer, "{\"version\":\"");
        write_literal(writer, status.version);
        write_literal(writer, "\",\"profile_index\":");
        write_uint(writer, status.profile_index);
        write_literal(writer, ",\"layer_index\":");
        write_uint(writer, status.layer_index);
        write_literal(writer, ",\"battery\":");
        write_uint(writer, status.battery_percent);
        write_literal(writer, ",\"is_charging\":");
        write_literal(writer, status.is_charging ? "true" : "false");
        write_literal(writer, "}");
        break;
    }
    case METHOD_RGB_CONFIG:
    case METHOD_AGENT_STATUS:
    case METHOD_FOCUSED_APP:
        /* Side-effect-free compatibility ACK; Task 11 later owns RGB state. */
        write_literal(writer, "{\"ok\":1}");
        break;
    case METHOD_LIGHTS_PREVIEW:
    case METHOD_ACTIVE_SCREEN:
    case METHOD_HOME_ACCENT_COLOR:
        /* Stable query/preview ACKs only: no lighting or UI state is changed. */
        write_literal(writer, "null");
        break;
    default:
        return;
    }
    write_id_and_close(writer, id, method);
}

int codex_rpc_dispatch(enum codex_transport source, const uint8_t *json,
                       size_t len, codex_rpc_emit_t emit, void *ctx)
{
    struct request request = {0};
    struct writer writer = {0};
    const char *canonical = NULL;
    enum method_kind kind;
    int err;

    if ((unsigned int)source >= CODEX_TRANSPORT_COUNT || json == NULL ||
        emit == NULL || len == 0U) {
        return -EINVAL;
    }
    if (len > CODEX_JSON_MAX_SIZE) {
        return -EMSGSIZE;
    }
    err = parse_request(json, len, &request);
    if (err != 0) {
        return err;
    }
    kind = identify_method(request.method, &canonical);
    if (!request.has_id) {
        return 0;
    }
    if (kind == METHOD_UNKNOWN) {
        write_error(&writer, CODEX_RPC_ERROR_METHOD_NOT_FOUND, "Method not found",
                    request.id);
    } else if (kind == METHOD_FORBIDDEN) {
        write_error(&writer, CODEX_RPC_ERROR_FORBIDDEN, "Method forbidden",
                    request.id);
    } else {
        write_success(&writer, kind, canonical, request.id);
    }
    if (writer.error != 0) {
        return writer.error;
    }
    return emit(source, writer.data, writer.len, ctx);
}
