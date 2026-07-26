#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#define CODEX_LAYER_COUNT 6U
#define CODEX_HOLD_MS 3000U
#define CODEX_CONNECTION_IDLE_MS 5000U

/* The three monochrome outputs are ordered physically from top to bottom. */
#define CODEX_INDICATOR_TOP BIT(2)
#define CODEX_INDICATOR_MIDDLE BIT(1)
#define CODEX_INDICATOR_BOTTOM BIT(0)

enum codex_connection_choice {
    CODEX_CONNECTION_BLE_1 = 0,
    CODEX_CONNECTION_BLE_2,
    CODEX_CONNECTION_BLE_3,
    CODEX_CONNECTION_USB,
    CODEX_CONNECTION_CHOICE_COUNT,
};

struct codex_touch_diagnostics {
    uint32_t queue_full;
    uint32_t invalid_time;
    uint32_t layer_api_errors;
    uint32_t profile_api_errors;
};

void codex_touch_edge(bool touched, int64_t now_ms);
void codex_connection_tick(int64_t now_ms);

bool codex_connection_mode_active(void);
enum codex_connection_choice codex_connection_choice_get(void);
uint8_t codex_indicator_bits(void);
struct codex_touch_diagnostics codex_touch_diagnostics_get(void);

uint8_t codex_layer_indicator_bits(uint8_t layer);
uint8_t codex_connection_indicator_bits(enum codex_connection_choice choice);

/* Task 13 supplies a strong GPIO-backed implementation. */
void codex_indicator_sink(uint8_t bits);

#if defined(CONFIG_ZTEST)
void codex_touch_test_reset(void);
void codex_touch_test_drain(void);
void codex_touch_test_set_uptime(int64_t now_ms);
void codex_touch_test_fire_deadline(void);
#endif
