#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/led_strip.h>

#include <codex/lighting.h>

#if __has_include(<zmk/activity.h>)
#include <zmk/activity.h>
#else
/* Standalone native fixtures mirror the audited pinned-ZMK v0.3 contract. */
enum zmk_activity_state { ZMK_ACTIVITY_ACTIVE, ZMK_ACTIVITY_IDLE, ZMK_ACTIVITY_SLEEP };
#endif

#define CODEX_RGB_ZONE_COUNT (2U + CODEX_AGENT_COUNT)
#define CODEX_RENDERER_MAX_PIXELS 256U

enum codex_rgb_zone_id {
    CODEX_RGB_ZONE_KEYS = 0,
    CODEX_RGB_ZONE_AMBIENT,
    CODEX_RGB_ZONE_AGENT_0,
    CODEX_RGB_ZONE_AGENT_1,
    CODEX_RGB_ZONE_AGENT_2,
    CODEX_RGB_ZONE_AGENT_3,
    CODEX_RGB_ZONE_AGENT_4,
    CODEX_RGB_ZONE_AGENT_5,
};

struct codex_rgb_zone_map {
    const uint16_t* pixels;
    size_t count;
};

struct codex_rgb_layout {
    size_t pixel_count;
    struct codex_rgb_zone_map zones[CODEX_RGB_ZONE_COUNT];
};

int codex_renderer_render_model(const struct codex_lighting_model* model,
    const struct codex_rgb_layout* layout, int64_t now_ms, bool output_enabled,
    struct led_rgb* pixels, size_t count);

/* Takes a fresh Task 11 snapshot and uses the Task 13 layout seam. */
void codex_renderer_frame(int64_t now_ms, struct led_rgb* pixels, size_t count);
void codex_renderer_on_activity(enum zmk_activity_state state);

/* Task 13 supplies the Devicetree-derived layout and standard strip sink. */
const struct codex_rgb_layout* codex_renderer_layout(void);
int codex_renderer_output_frame(
    const struct codex_lighting_model* model, int64_t now_ms, bool output_enabled);

#if defined(CONFIG_ZTEST)
void codex_renderer_test_reset(void);
void codex_renderer_test_drain(void);
void codex_renderer_test_set_auto_off(bool enabled);
#endif
