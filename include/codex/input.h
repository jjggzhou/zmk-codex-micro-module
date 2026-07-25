#pragma once

#include <stdbool.h>
#include <stdint.h>

int codex_input_key(const char *id, bool pressed, uint8_t agent_index);
int codex_input_encoder_press(bool pressed);
int codex_input_encoder_tick(int direction);

struct codex_radial {
    float angle;
    float distance;
};

/* Sends a bounded radial sample through the analog input worker. */
int codex_input_radial_emit(struct codex_radial value);

#if defined(CONFIG_ZTEST)
struct codex_analog_calibration {
    int32_t center_x;
    int32_t center_y;
    int32_t max_x;
    int32_t max_y;
    int32_t dead_zone;
    bool invert_x;
    bool invert_y;
    uint16_t meaningful_delta;
    uint32_t refresh_interval_ms;
};

struct codex_radial codex_analog_normalize(int32_t raw_x, int32_t raw_y);
int codex_analog_test_configure(const struct codex_analog_calibration *calibration);
int codex_analog_test_input_event(uint16_t code, int32_t value, bool sync);
void codex_analog_test_reset(void);
void codex_analog_test_set_uptime(uint32_t uptime_ms);
#endif

#if defined(CONFIG_ZTEST)
void codex_input_test_reset(void);
#endif
