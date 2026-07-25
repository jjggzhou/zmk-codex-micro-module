#pragma once

#include <stddef.h>
#include <stdint.h>

enum codex_json_result {
    CODEX_JSON_INVALID = -1,
    CODEX_JSON_INCOMPLETE = 0,
    CODEX_JSON_COMPLETE = 1,
};

enum codex_json_root_kind {
    CODEX_JSON_ROOT_OTHER,
    CODEX_JSON_ROOT_NUMBER,
};

struct codex_json_scan {
    enum codex_json_result result;
    enum codex_json_root_kind root_kind;
    size_t value_end;
};

struct codex_json_scan codex_json_value_scan(const uint8_t *data, size_t len,
                                             size_t maximum_depth);

enum codex_json_result codex_json_utf8_validate(const uint8_t *data, size_t len);

enum codex_json_result codex_json_value_validate(const uint8_t *data, size_t len,
                                                 size_t maximum_depth);
