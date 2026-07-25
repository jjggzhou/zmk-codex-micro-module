#pragma once

#include <stddef.h>
#include <stdint.h>

enum codex_json_result {
    CODEX_JSON_INVALID = -1,
    CODEX_JSON_INCOMPLETE = 0,
    CODEX_JSON_COMPLETE = 1,
};

enum codex_json_result codex_json_value_validate(const uint8_t *data, size_t len,
                                                 size_t maximum_depth);
