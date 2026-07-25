#include "json_value.h"

#include <stdbool.h>

struct parser {
    const uint8_t *data;
    size_t len;
    size_t pos;
    size_t maximum_depth;
};

static bool is_json_space(uint8_t byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

static void skip_space(struct parser *parser)
{
    while (parser->pos < parser->len && is_json_space(parser->data[parser->pos])) {
        parser->pos++;
    }
}

static enum codex_json_result validate_utf8(const uint8_t *data, size_t len)
{
    for (size_t i = 0U; i < len;) {
        uint8_t first = data[i++];
        size_t continuation_count;
        uint8_t second_min = 0x80U;
        uint8_t second_max = 0xBFU;

        if (first <= 0x7FU) {
            continue;
        }
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2U;
            if (first == 0xE0U) {
                second_min = 0xA0U;
            } else if (first == 0xEDU) {
                second_max = 0x9FU;
            }
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3U;
            if (first == 0xF0U) {
                second_min = 0x90U;
            } else if (first == 0xF4U) {
                second_max = 0x8FU;
            }
        } else {
            return CODEX_JSON_INVALID;
        }

        if (len - i < continuation_count) {
            return CODEX_JSON_INCOMPLETE;
        }
        if (data[i] < second_min || data[i] > second_max) {
            return CODEX_JSON_INVALID;
        }
        for (size_t j = 1U; j < continuation_count; j++) {
            if (data[i + j] < 0x80U || data[i + j] > 0xBFU) {
                return CODEX_JSON_INVALID;
            }
        }
        i += continuation_count;
    }

    return CODEX_JSON_COMPLETE;
}

static int hex_value(uint8_t byte)
{
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

static enum codex_json_result parse_hex4(struct parser *parser, uint16_t *value)
{
    uint16_t result = 0U;

    if (parser->len - parser->pos < 4U) {
        return CODEX_JSON_INCOMPLETE;
    }
    for (size_t i = 0U; i < 4U; i++) {
        int digit = hex_value(parser->data[parser->pos++]);

        if (digit < 0) {
            return CODEX_JSON_INVALID;
        }
        result = (uint16_t)((result << 4U) | (uint16_t)digit);
    }
    *value = result;
    return CODEX_JSON_COMPLETE;
}

static enum codex_json_result parse_string(struct parser *parser)
{
    if (parser->pos >= parser->len) {
        return CODEX_JSON_INCOMPLETE;
    }
    if (parser->data[parser->pos++] != '"') {
        return CODEX_JSON_INVALID;
    }

    while (parser->pos < parser->len) {
        uint8_t byte = parser->data[parser->pos++];

        if (byte == '"') {
            return CODEX_JSON_COMPLETE;
        }
        if (byte < 0x20U) {
            return CODEX_JSON_INVALID;
        }
        if (byte != '\\') {
            continue;
        }
        if (parser->pos >= parser->len) {
            return CODEX_JSON_INCOMPLETE;
        }

        byte = parser->data[parser->pos++];
        if (byte == '"' || byte == '\\' || byte == '/' || byte == 'b' ||
            byte == 'f' || byte == 'n' || byte == 'r' || byte == 't') {
            continue;
        }
        if (byte != 'u') {
            return CODEX_JSON_INVALID;
        }

        uint16_t first;
        enum codex_json_result result = parse_hex4(parser, &first);

        if (result != CODEX_JSON_COMPLETE) {
            return result;
        }
        if (first >= 0xDC00U && first <= 0xDFFFU) {
            return CODEX_JSON_INVALID;
        }
        if (first >= 0xD800U && first <= 0xDBFFU) {
            uint16_t second;

            if (parser->len - parser->pos < 2U) {
                return CODEX_JSON_INCOMPLETE;
            }
            if (parser->data[parser->pos] != '\\' ||
                parser->data[parser->pos + 1U] != 'u') {
                return CODEX_JSON_INVALID;
            }
            parser->pos += 2U;
            result = parse_hex4(parser, &second);
            if (result != CODEX_JSON_COMPLETE) {
                return result;
            }
            if (second < 0xDC00U || second > 0xDFFFU) {
                return CODEX_JSON_INVALID;
            }
        }
    }

    return CODEX_JSON_INCOMPLETE;
}

static enum codex_json_result parse_value(struct parser *parser, size_t depth);

static enum codex_json_result parse_array(struct parser *parser, size_t depth)
{
    enum codex_json_result result;

    if (depth > parser->maximum_depth) {
        return CODEX_JSON_INVALID;
    }
    parser->pos++;
    skip_space(parser);
    if (parser->pos >= parser->len) {
        return CODEX_JSON_INCOMPLETE;
    }
    if (parser->data[parser->pos] == ']') {
        parser->pos++;
        return CODEX_JSON_COMPLETE;
    }

    for (;;) {
        result = parse_value(parser, depth);
        if (result != CODEX_JSON_COMPLETE) {
            return result;
        }
        skip_space(parser);
        if (parser->pos >= parser->len) {
            return CODEX_JSON_INCOMPLETE;
        }
        if (parser->data[parser->pos] == ']') {
            parser->pos++;
            return CODEX_JSON_COMPLETE;
        }
        if (parser->data[parser->pos++] != ',') {
            return CODEX_JSON_INVALID;
        }
        skip_space(parser);
        if (parser->pos >= parser->len) {
            return CODEX_JSON_INCOMPLETE;
        }
    }
}

static enum codex_json_result parse_object(struct parser *parser, size_t depth)
{
    enum codex_json_result result;

    if (depth > parser->maximum_depth) {
        return CODEX_JSON_INVALID;
    }
    parser->pos++;
    skip_space(parser);
    if (parser->pos >= parser->len) {
        return CODEX_JSON_INCOMPLETE;
    }
    if (parser->data[parser->pos] == '}') {
        parser->pos++;
        return CODEX_JSON_COMPLETE;
    }

    for (;;) {
        result = parse_string(parser);
        if (result != CODEX_JSON_COMPLETE) {
            return result;
        }
        skip_space(parser);
        if (parser->pos >= parser->len) {
            return CODEX_JSON_INCOMPLETE;
        }
        if (parser->data[parser->pos++] != ':') {
            return CODEX_JSON_INVALID;
        }
        result = parse_value(parser, depth);
        if (result != CODEX_JSON_COMPLETE) {
            return result;
        }
        skip_space(parser);
        if (parser->pos >= parser->len) {
            return CODEX_JSON_INCOMPLETE;
        }
        if (parser->data[parser->pos] == '}') {
            parser->pos++;
            return CODEX_JSON_COMPLETE;
        }
        if (parser->data[parser->pos++] != ',') {
            return CODEX_JSON_INVALID;
        }
        skip_space(parser);
        if (parser->pos >= parser->len) {
            return CODEX_JSON_INCOMPLETE;
        }
    }
}

static enum codex_json_result parse_literal(struct parser *parser, const char *literal,
                                            size_t literal_len)
{
    size_t available = parser->len - parser->pos;
    size_t compare_len = available < literal_len ? available : literal_len;

    for (size_t i = 0U; i < compare_len; i++) {
        if (parser->data[parser->pos + i] != (uint8_t)literal[i]) {
            return CODEX_JSON_INVALID;
        }
    }
    if (available < literal_len) {
        parser->pos = parser->len;
        return CODEX_JSON_INCOMPLETE;
    }
    parser->pos += literal_len;
    return CODEX_JSON_COMPLETE;
}

static bool is_digit(uint8_t byte)
{
    return byte >= '0' && byte <= '9';
}

static enum codex_json_result parse_number(struct parser *parser)
{
    if (parser->data[parser->pos] == '-') {
        parser->pos++;
        if (parser->pos >= parser->len) {
            return CODEX_JSON_INCOMPLETE;
        }
    }

    if (parser->data[parser->pos] == '0') {
        parser->pos++;
        if (parser->pos < parser->len && is_digit(parser->data[parser->pos])) {
            return CODEX_JSON_INVALID;
        }
    } else if (parser->data[parser->pos] >= '1' && parser->data[parser->pos] <= '9') {
        do {
            parser->pos++;
        } while (parser->pos < parser->len && is_digit(parser->data[parser->pos]));
    } else {
        return CODEX_JSON_INVALID;
    }

    if (parser->pos < parser->len && parser->data[parser->pos] == '.') {
        parser->pos++;
        if (parser->pos >= parser->len) {
            return CODEX_JSON_INCOMPLETE;
        }
        if (!is_digit(parser->data[parser->pos])) {
            return CODEX_JSON_INVALID;
        }
        do {
            parser->pos++;
        } while (parser->pos < parser->len && is_digit(parser->data[parser->pos]));
    }

    if (parser->pos < parser->len &&
        (parser->data[parser->pos] == 'e' || parser->data[parser->pos] == 'E')) {
        parser->pos++;
        if (parser->pos >= parser->len) {
            return CODEX_JSON_INCOMPLETE;
        }
        if (parser->data[parser->pos] == '+' || parser->data[parser->pos] == '-') {
            parser->pos++;
            if (parser->pos >= parser->len) {
                return CODEX_JSON_INCOMPLETE;
            }
        }
        if (!is_digit(parser->data[parser->pos])) {
            return CODEX_JSON_INVALID;
        }
        do {
            parser->pos++;
        } while (parser->pos < parser->len && is_digit(parser->data[parser->pos]));
    }

    return CODEX_JSON_COMPLETE;
}

static enum codex_json_result parse_value(struct parser *parser, size_t depth)
{
    skip_space(parser);
    if (parser->pos >= parser->len) {
        return CODEX_JSON_INCOMPLETE;
    }

    switch (parser->data[parser->pos]) {
    case '{':
        return parse_object(parser, depth + 1U);
    case '[':
        return parse_array(parser, depth + 1U);
    case '"':
        return parse_string(parser);
    case 't':
        return parse_literal(parser, "true", 4U);
    case 'f':
        return parse_literal(parser, "false", 5U);
    case 'n':
        return parse_literal(parser, "null", 4U);
    default:
        return parse_number(parser);
    }
}

enum codex_json_result codex_json_value_validate(const uint8_t *data, size_t len,
                                                 size_t maximum_depth)
{
    struct parser parser = {
        .data = data,
        .len = len,
        .maximum_depth = maximum_depth,
    };
    enum codex_json_result result;

    if (data == NULL || len == 0U || maximum_depth == 0U) {
        return len == 0U ? CODEX_JSON_INCOMPLETE : CODEX_JSON_INVALID;
    }
    result = validate_utf8(data, len);
    if (result != CODEX_JSON_COMPLETE) {
        return result;
    }
    result = parse_value(&parser, 0U);
    if (result != CODEX_JSON_COMPLETE) {
        return result;
    }
    skip_space(&parser);
    return parser.pos == parser.len ? CODEX_JSON_COMPLETE : CODEX_JSON_INVALID;
}
