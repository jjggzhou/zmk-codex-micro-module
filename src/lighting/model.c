#include <codex/lighting.h>
#include <codex/framing.h>

#include "json_value.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

enum zone_field {
    ZONE_FIELD_EFFECT = BIT(0),
    ZONE_FIELD_BRIGHTNESS = BIT(1),
    ZONE_FIELD_SPEED = BIT(2),
    ZONE_FIELD_COLOR = BIT(3),
    ZONE_FIELD_MODE = BIT(4),
};

enum agent_field {
    AGENT_FIELD_ID = BIT(0),
    AGENT_FIELD_SK = BIT(1),
    AGENT_FIELD_SA = BIT(2),
};

struct span {
    const uint8_t *data;
    size_t len;
};

struct cursor {
    const uint8_t *data;
    size_t len;
    size_t pos;
};

struct zone_patch {
    struct codex_lighting_zone value;
    uint8_t fields;
};

struct agent_patch {
    struct zone_patch zone;
    uint8_t id;
    uint8_t fields;
    bool sk;
    bool sa;
};

static struct codex_lighting_model lighting_model;
static struct k_spinlock lighting_lock;

static bool is_space(uint8_t byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

static void skip_space(struct cursor *cursor)
{
    while (cursor->pos < cursor->len && is_space(cursor->data[cursor->pos])) {
        cursor->pos++;
    }
}

static void trim_space(const uint8_t **data, size_t *len)
{
    while (*len > 0U && is_space(**data)) {
        (*data)++;
        (*len)--;
    }
    while (*len > 0U && is_space((*data)[*len - 1U])) {
        (*len)--;
    }
}

static size_t skip_string(const uint8_t *data, size_t pos)
{
    pos++;
    while (data[pos] != '"') {
        if (data[pos++] == '\\') {
            if (data[pos++] == 'u') {
                pos += 4U;
            }
        }
    }
    return pos + 1U;
}

static size_t skip_value(const uint8_t *data, size_t len, size_t pos)
{
    if (data[pos] == '"') {
        return skip_string(data, pos);
    }
    if (data[pos] == '{' || data[pos] == '[') {
        uint8_t stack[32];
        size_t depth = 0U;

        do {
            if (data[pos] == '"') {
                pos = skip_string(data, pos);
                continue;
            }
            if (data[pos] == '{' || data[pos] == '[') {
                stack[depth++] = data[pos] == '{' ? '}' : ']';
            } else if (data[pos] == stack[depth - 1U]) {
                depth--;
            }
            pos++;
        } while (depth > 0U);
        return pos;
    }
    while (pos < len && data[pos] != ',' && data[pos] != '}' &&
           data[pos] != ']' && !is_space(data[pos])) {
        pos++;
    }
    return pos;
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

static uint16_t hex4(const uint8_t *data)
{
    uint16_t value = 0U;

    for (size_t i = 0U; i < 4U; i++) {
        value = (uint16_t)((value << 4U) | (uint16_t)hex_value(data[i]));
    }
    return value;
}

static int string_codepoint(struct span string, size_t *pos, uint32_t *codepoint)
{
    if (*pos >= string.len - 1U) {
        return 0;
    }

    uint8_t byte = string.data[(*pos)++];

    if (byte != '\\') {
        if (byte < 0x80U) {
            *codepoint = byte;
            return 1;
        }
        size_t count = byte < 0xE0U ? 1U : byte < 0xF0U ? 2U : 3U;
        uint32_t value = byte & (0x7FU >> count);

        for (size_t i = 0U; i < count; i++) {
            value = (value << 6U) | (string.data[(*pos)++] & 0x3FU);
        }
        *codepoint = value;
        return 1;
    }

    byte = string.data[(*pos)++];
    switch (byte) {
    case '"': case '\\': case '/':
        *codepoint = byte;
        return 1;
    case 'b': *codepoint = '\b'; return 1;
    case 'f': *codepoint = '\f'; return 1;
    case 'n': *codepoint = '\n'; return 1;
    case 'r': *codepoint = '\r'; return 1;
    case 't': *codepoint = '\t'; return 1;
    default: {
        uint16_t first = hex4(&string.data[*pos]);

        *pos += 4U;
        if (first >= 0xD800U && first <= 0xDBFFU) {
            *pos += 2U;
            uint16_t second = hex4(&string.data[*pos]);

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

static bool string_is(struct span string, const char *literal)
{
    size_t pos = 1U;
    size_t literal_pos = 0U;
    uint32_t codepoint;

    if (string.len < 2U || string.data[0] != '"' ||
        string.data[string.len - 1U] != '"') {
        return false;
    }
    while (string_codepoint(string, &pos, &codepoint) != 0) {
        if (codepoint > UINT8_MAX || literal[literal_pos] == '\0' ||
            codepoint != (uint8_t)literal[literal_pos]) {
            return false;
        }
        literal_pos++;
    }
    return literal[literal_pos] == '\0';
}

static int next_member(struct cursor *cursor, struct span *key, struct span *value,
                       uint8_t close)
{
    size_t start;

    skip_space(cursor);
    if (cursor->pos >= cursor->len || cursor->data[cursor->pos] != '"') {
        return -EINVAL;
    }
    start = cursor->pos;
    cursor->pos = skip_string(cursor->data, cursor->pos);
    *key = (struct span){&cursor->data[start], cursor->pos - start};
    skip_space(cursor);
    if (cursor->pos >= cursor->len || cursor->data[cursor->pos++] != ':') {
        return -EINVAL;
    }
    skip_space(cursor);
    start = cursor->pos;
    cursor->pos = skip_value(cursor->data, cursor->len, cursor->pos);
    *value = (struct span){&cursor->data[start], cursor->pos - start};
    skip_space(cursor);
    if (cursor->pos >= cursor->len ||
        (cursor->data[cursor->pos] != ',' && cursor->data[cursor->pos] != close)) {
        return -EINVAL;
    }
    return 0;
}

static int number_value(struct span span, double *result)
{
    size_t pos = 0U;
    size_t digits = 0U;
    bool negative = false;
    double value = 0.0;
    double fraction = 0.1;
    int exponent = 0;
    bool exponent_negative = false;

    if (span.len == 0U) {
        return -EINVAL;
    }
    if (span.data[pos] == '-') {
        negative = true;
        pos++;
    }
    if (pos >= span.len || span.data[pos] < '0' || span.data[pos] > '9') {
        return -EINVAL;
    }
    while (pos < span.len && span.data[pos] >= '0' && span.data[pos] <= '9') {
        uint8_t digit = span.data[pos++] - '0';

        if (value > (DBL_MAX - (double)digit) / 10.0) {
            return -ERANGE;
        }
        value = value * 10.0 + (double)digit;
        digits++;
    }
    if (pos < span.len && span.data[pos] == '.') {
        pos++;
        while (pos < span.len && span.data[pos] >= '0' && span.data[pos] <= '9') {
            value += (double)(span.data[pos++] - '0') * fraction;
            fraction *= 0.1;
        }
    }
    if (pos < span.len && (span.data[pos] == 'e' || span.data[pos] == 'E')) {
        pos++;
        if (pos >= span.len) {
            return -EINVAL;
        }
        if (span.data[pos] == '+' || span.data[pos] == '-') {
            exponent_negative = span.data[pos++] == '-';
        }
        if (pos >= span.len || span.data[pos] < '0' || span.data[pos] > '9') {
            return -EINVAL;
        }
        while (pos < span.len) {
            if (span.data[pos] < '0' || span.data[pos] > '9') {
                return -EINVAL;
            }
            int digit = span.data[pos] - '0';

            if (exponent <= 400) {
                exponent = exponent > 40 || exponent * 10 + digit > 400
                               ? 401
                               : exponent * 10 + digit;
            }
            pos++;
        }
    }
    if (digits == 0U || pos != span.len) {
        return -EINVAL;
    }
    if (exponent > 400) {
        if (!exponent_negative && value != 0.0) {
            return -ERANGE;
        }
        value = 0.0;
    } else {
        while (exponent-- > 0) {
            if (exponent_negative) {
                value /= 10.0;
            } else {
                if (value > DBL_MAX / 10.0) {
                    return -ERANGE;
                }
                value *= 10.0;
            }
        }
    }
    *result = negative ? -value : value;
    return 0;
}

static bool number_is_integer(struct span span)
{
    size_t mantissa_end = span.len;
    size_t mantissa_digits = 0U;
    size_t fractional_digits = 0U;
    size_t pos = span.data[0] == '-' ? 1U : 0U;
    bool after_decimal = false;

    for (; pos < span.len; pos++) {
        uint8_t byte = span.data[pos];

        if (byte >= '0' && byte <= '9') {
            mantissa_digits++;
            if (after_decimal) {
                fractional_digits++;
            }
        } else if (byte == '.') {
            after_decimal = true;
        } else {
            mantissa_end = pos;
            break;
        }
    }

    bool exponent_negative = false;
    size_t exponent = 0U;

    if (mantissa_end < span.len) {
        pos = mantissa_end + 1U;
        if (span.data[pos] == '+' || span.data[pos] == '-') {
            exponent_negative = span.data[pos++] == '-';
        }
        for (; pos < span.len; pos++) {
            size_t digit = span.data[pos] - '0';

            if (exponent <= CODEX_JSON_MAX_SIZE) {
                exponent = exponent * 10U + digit;
            }
        }
    }

    size_t scale;

    if (!exponent_negative) {
        if (exponent >= fractional_digits) {
            return true;
        }
        scale = fractional_digits - exponent;
    } else if (exponent > CODEX_JSON_MAX_SIZE) {
        scale = mantissa_digits;
    } else {
        scale = fractional_digits + exponent;
        if (scale > mantissa_digits) {
            scale = mantissa_digits;
        }
    }

    size_t remaining = MIN(scale, mantissa_digits);

    for (pos = mantissa_end; pos > 0U && remaining > 0U;) {
        uint8_t byte = span.data[--pos];

        if (byte >= '0' && byte <= '9') {
            if (byte != '0') {
                return false;
            }
            remaining--;
        }
    }
    return true;
}

struct exact_integer {
    uint32_t magnitude;
    bool negative;
    bool above_maximum;
};

static int exact_integer_value(struct span span, uint32_t maximum,
                               struct exact_integer *result)
{
    double parsed;

    if (number_value(span, &parsed) != 0 || !number_is_integer(span)) {
        return -EINVAL;
    }

    bool negative = span.data[0] == '-';
    size_t pos = negative ? 1U : 0U;
    size_t mantissa_end = span.len;
    size_t mantissa_digits = 0U;
    size_t fractional_digits = 0U;
    bool after_decimal = false;

    for (; pos < span.len; pos++) {
        uint8_t byte = span.data[pos];

        if (byte >= '0' && byte <= '9') {
            mantissa_digits++;
            if (after_decimal) {
                fractional_digits++;
            }
        } else if (byte == '.') {
            after_decimal = true;
        } else {
            mantissa_end = pos;
            break;
        }
    }

    bool exponent_negative = false;
    size_t exponent = 0U;

    if (mantissa_end < span.len) {
        pos = mantissa_end + 1U;
        if (span.data[pos] == '+' || span.data[pos] == '-') {
            exponent_negative = span.data[pos++] == '-';
        }
        for (; pos < span.len; pos++) {
            size_t digit = span.data[pos] - '0';

            if (exponent <= CODEX_JSON_MAX_SIZE) {
                exponent = exponent > CODEX_JSON_MAX_SIZE / 10U ||
                                   exponent * 10U + digit > CODEX_JSON_MAX_SIZE
                               ? CODEX_JSON_MAX_SIZE + 1U
                               : exponent * 10U + digit;
            }
        }
    }

    size_t keep_digits;
    size_t append_zeros = 0U;

    if (exponent_negative) {
        size_t scale = exponent > CODEX_JSON_MAX_SIZE
                           ? mantissa_digits
                           : fractional_digits + exponent;

        keep_digits = scale >= mantissa_digits ? 0U : mantissa_digits - scale;
    } else if (exponent >= fractional_digits) {
        keep_digits = mantissa_digits;
        append_zeros = exponent - fractional_digits;
    } else {
        keep_digits = mantissa_digits - (fractional_digits - exponent);
    }

    uint32_t magnitude = 0U;
    size_t consumed = 0U;

    for (pos = negative ? 1U : 0U; pos < mantissa_end && consumed < keep_digits;
         pos++) {
        uint8_t byte = span.data[pos];

        if (byte < '0' || byte > '9') {
            continue;
        }
        uint32_t digit = byte - '0';

        if (digit > maximum || magnitude > (maximum - digit) / 10U) {
            result->above_maximum = true;
            break;
        }
        magnitude = magnitude * 10U + digit;
        consumed++;
    }
    while (!result->above_maximum && append_zeros-- > 0U && magnitude != 0U) {
        if (magnitude > maximum / 10U) {
            result->above_maximum = true;
            break;
        }
        magnitude *= 10U;
    }

    result->magnitude = magnitude;
    result->negative = negative;
    return 0;
}

static int bounded_integer(struct span span, int minimum, int maximum, int *result)
{
    struct exact_integer exact = {0};

    if (exact_integer_value(span, (uint32_t)maximum, &exact) != 0 ||
        exact.above_maximum) {
        return -EINVAL;
    }

    int integer = exact.negative ? -(int)exact.magnitude : (int)exact.magnitude;

    if (integer < minimum || integer > maximum) {
        return -EINVAL;
    }
    *result = integer;
    return 0;
}

static float clamp_unit(double value)
{
    if (value <= 0.0) {
        return 0.0f;
    }
    if (value >= 1.0) {
        return 1.0f;
    }
    return (float)value;
}

static int parse_zone_field(struct span key, struct span value, bool allow_mode,
                            struct zone_patch *patch)
{
    uint8_t field;
    double number;
    int integer;

    if (string_is(key, "e")) {
        field = ZONE_FIELD_EFFECT;
        if (bounded_integer(value, CODEX_EFFECT_OFF,
                            CODEX_EFFECT_SHALLOW_BREATH, &integer) != 0) {
            return -EINVAL;
        }
        patch->value.effect = (enum codex_effect)integer;
    } else if (string_is(key, "b")) {
        field = ZONE_FIELD_BRIGHTNESS;
        if (number_value(value, &number) != 0) {
            return -EINVAL;
        }
        patch->value.brightness = clamp_unit(number);
    } else if (string_is(key, "s")) {
        field = ZONE_FIELD_SPEED;
        if (number_value(value, &number) != 0) {
            return -EINVAL;
        }
        patch->value.speed = clamp_unit(number);
    } else if (string_is(key, "c")) {
        struct exact_integer color = {0};

        field = ZONE_FIELD_COLOR;
        if (exact_integer_value(value, 0xFFFFFFU, &color) != 0) {
            return -EINVAL;
        }
        patch->value.color = color.negative ? 0U :
                             color.above_maximum ? 0xFFFFFFU : color.magnitude;
    } else if (allow_mode && string_is(key, "m")) {
        field = ZONE_FIELD_MODE;
        if (number_value(value, &number) != 0) {
            return -EINVAL;
        }
        patch->value.mode = clamp_unit(number);
    } else {
        return -EINVAL;
    }
    if ((patch->fields & field) != 0U) {
        return -EINVAL;
    }
    patch->fields |= field;
    return 0;
}

static int parse_zone(struct span object, bool allow_mode, struct zone_patch *patch)
{
    struct cursor cursor = {object.data, object.len, 1U};

    if (object.len < 2U || object.data[0] != '{' ||
        object.data[object.len - 1U] != '}') {
        return -EINVAL;
    }
    skip_space(&cursor);
    if (cursor.data[cursor.pos] == '}') {
        return -EINVAL;
    }
    while (cursor.pos < cursor.len && cursor.data[cursor.pos] != '}') {
        struct span key;
        struct span value;
        if (next_member(&cursor, &key, &value, '}') != 0) {
            return -EINVAL;
        }
        if (parse_zone_field(key, value, allow_mode, patch) != 0) {
            return -EINVAL;
        }
        if (cursor.data[cursor.pos] == ',') {
            cursor.pos++;
            skip_space(&cursor);
        }
    }
    return patch->fields == 0U ? -EINVAL : 0;
}

static void apply_zone_patch(struct codex_lighting_zone *zone,
                             const struct zone_patch *patch)
{
    if ((patch->fields & ZONE_FIELD_EFFECT) != 0U) {
        zone->effect = patch->value.effect;
    }
    if ((patch->fields & ZONE_FIELD_BRIGHTNESS) != 0U) {
        zone->brightness = patch->value.brightness;
    }
    if ((patch->fields & ZONE_FIELD_SPEED) != 0U) {
        zone->speed = patch->value.speed;
    }
    if ((patch->fields & ZONE_FIELD_COLOR) != 0U) {
        zone->color = patch->value.color;
    }
    if ((patch->fields & ZONE_FIELD_MODE) != 0U) {
        zone->mode = patch->value.mode;
    }
}

static int parse_agent(struct span object, struct agent_patch *patch)
{
    struct cursor cursor = {object.data, object.len, 1U};

    if (object.len < 2U || object.data[0] != '{' ||
        object.data[object.len - 1U] != '}') {
        return -EINVAL;
    }
    skip_space(&cursor);
    if (cursor.data[cursor.pos] == '}') {
        return -EINVAL;
    }
    while (cursor.pos < cursor.len && cursor.data[cursor.pos] != '}') {
        struct span key;
        struct span value;
        uint8_t field;
        int integer;

        if (next_member(&cursor, &key, &value, '}') != 0) {
            return -EINVAL;
        }
        if (string_is(key, "id")) {
            field = AGENT_FIELD_ID;
            int id;

            if (bounded_integer(value, 0, CODEX_AGENT_COUNT - 1, &id) != 0) {
                return -EINVAL;
            }
            patch->id = (uint8_t)id;
        } else if (string_is(key, "sk") || string_is(key, "sa")) {
            field = string_is(key, "sk") ? AGENT_FIELD_SK : AGENT_FIELD_SA;
            if (bounded_integer(value, 0, 1, &integer) != 0) {
                return -EINVAL;
            }
            if ((patch->fields & field) != 0U) {
                return -EINVAL;
            }
            if (field == AGENT_FIELD_SK) {
                patch->sk = integer != 0;
            } else {
                patch->sa = integer != 0;
            }
            patch->fields |= field;
            if (cursor.data[cursor.pos] == ',') {
                cursor.pos++;
                skip_space(&cursor);
            }
            continue;
        } else {
            if (parse_zone_field(key, value, false, &patch->zone) != 0) {
                return -EINVAL;
            }
            if (cursor.data[cursor.pos] == ',') {
                cursor.pos++;
                skip_space(&cursor);
            }
            continue;
        }
        if ((patch->fields & field) != 0U) {
            return -EINVAL;
        }
        patch->fields |= field;
        if (cursor.data[cursor.pos] == ',') {
            cursor.pos++;
            skip_space(&cursor);
        }
    }
    if ((patch->fields & AGENT_FIELD_ID) == 0U ||
        (patch->zone.fields == 0U &&
         (patch->fields & (AGENT_FIELD_SK | AGENT_FIELD_SA)) == 0U)) {
        return -EINVAL;
    }
    return 0;
}

__weak void codex_lighting_changed(void) {}

#if defined(CONFIG_ZTEST)
__weak void codex_lighting_test_before_commit(void) {}
#endif

struct codex_lighting_model codex_lighting_snapshot(void)
{
    k_spinlock_key_t key = k_spin_lock(&lighting_lock);
    struct codex_lighting_model snapshot = lighting_model;

    k_spin_unlock(&lighting_lock, key);
    return snapshot;
}

int codex_lighting_apply_rgbcfg(const uint8_t *params, size_t len)
{
    struct zone_patch keys = {0};
    struct zone_patch ambient = {0};
    uint8_t zones = 0U;

    if (params == NULL || len == 0U) {
        return -EINVAL;
    }
    if (len > CODEX_JSON_MAX_SIZE) {
        return -EMSGSIZE;
    }
    trim_space(&params, &len);
    if (len == 0U ||
        codex_json_value_validate(params, len, CODEX_JSON_MAX_DEPTH) !=
            CODEX_JSON_COMPLETE ||
        params[0] != '{' || params[len - 1U] != '}') {
        return -EINVAL;
    }
    struct cursor cursor = {params, len, 1U};

    skip_space(&cursor);
    if (cursor.data[cursor.pos] == '}') {
        return -EINVAL;
    }
    while (cursor.pos < cursor.len && cursor.data[cursor.pos] != '}') {
        struct span key;
        struct span value;
        uint8_t zone;
        struct zone_patch *patch;

        if (next_member(&cursor, &key, &value, '}') != 0) {
            return -EINVAL;
        }
        if (string_is(key, "keys")) {
            zone = BIT(0);
            patch = &keys;
        } else if (string_is(key, "ambient")) {
            zone = BIT(1);
            patch = &ambient;
        } else {
            return -EINVAL;
        }
        if ((zones & zone) != 0U || parse_zone(value, true, patch) != 0) {
            return -EINVAL;
        }
        zones |= zone;
        if (cursor.data[cursor.pos] == ',') {
            cursor.pos++;
            skip_space(&cursor);
        }
    }

    bool changed;
#if defined(CONFIG_ZTEST)
    codex_lighting_test_before_commit();
#endif
    k_spinlock_key_t lock_key = k_spin_lock(&lighting_lock);
    struct codex_lighting_model next = lighting_model;

    apply_zone_patch(&next.keys, &keys);
    apply_zone_patch(&next.ambient, &ambient);
    changed = memcmp(&next, &lighting_model, sizeof(next)) != 0;
    if (changed) {
        lighting_model = next;
    }
    k_spin_unlock(&lighting_lock, lock_key);
    if (changed) {
        codex_lighting_changed();
    }
    return 0;
}

int codex_lighting_apply_thstatus(const uint8_t *params, size_t len)
{
    struct agent_patch patches[CODEX_AGENT_COUNT] = {0};
    uint8_t ids = 0U;
    size_t count = 0U;

    if (params == NULL || len == 0U) {
        return -EINVAL;
    }
    if (len > CODEX_JSON_MAX_SIZE) {
        return -EMSGSIZE;
    }
    trim_space(&params, &len);
    if (len == 0U ||
        codex_json_value_validate(params, len, CODEX_JSON_MAX_DEPTH) !=
            CODEX_JSON_COMPLETE ||
        params[0] != '[' || params[len - 1U] != ']') {
        return -EINVAL;
    }
    struct cursor cursor = {params, len, 1U};

    skip_space(&cursor);
    if (cursor.data[cursor.pos] == ']') {
        return -EINVAL;
    }
    while (cursor.pos < cursor.len && cursor.data[cursor.pos] != ']') {
        size_t start = cursor.pos;
        size_t end = skip_value(cursor.data, cursor.len, start);

        if (count == ARRAY_SIZE(patches) ||
            parse_agent((struct span){&cursor.data[start], end - start},
                        &patches[count]) != 0 ||
            (ids & BIT(patches[count].id)) != 0U) {
            return -EINVAL;
        }
        ids |= BIT(patches[count].id);
        count++;
        cursor.pos = end;
        skip_space(&cursor);
        if (cursor.data[cursor.pos] == ',') {
            cursor.pos++;
            skip_space(&cursor);
        }
    }

    bool changed;
#if defined(CONFIG_ZTEST)
    codex_lighting_test_before_commit();
#endif
    k_spinlock_key_t lock_key = k_spin_lock(&lighting_lock);
    struct codex_lighting_model next = lighting_model;

    for (size_t i = 0U; i < count; i++) {
        struct codex_agent_lighting *agent = &next.agents[patches[i].id];

        apply_zone_patch(&agent->zone, &patches[i].zone);
        if ((patches[i].fields & AGENT_FIELD_SK) != 0U) {
            agent->sk = patches[i].sk;
        }
        if ((patches[i].fields & AGENT_FIELD_SA) != 0U) {
            agent->sa = patches[i].sa;
        }
    }
    changed = memcmp(&next, &lighting_model, sizeof(next)) != 0;
    if (changed) {
        lighting_model = next;
    }
    k_spin_unlock(&lighting_lock, lock_key);
    if (changed) {
        codex_lighting_changed();
    }
    return 0;
}

#if defined(CONFIG_ZTEST)
void codex_lighting_test_reset(void)
{
    k_spinlock_key_t key = k_spin_lock(&lighting_lock);

    memset(&lighting_model, 0, sizeof(lighting_model));
    k_spin_unlock(&lighting_lock, key);
}
#endif
