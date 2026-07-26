#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CODEX_AGENT_COUNT 6U

enum codex_effect {
    CODEX_EFFECT_OFF = 0,
    CODEX_EFFECT_SOLID = 1,
    CODEX_EFFECT_SNAKE = 2,
    CODEX_EFFECT_RAINBOW = 3,
    CODEX_EFFECT_BREATH = 4,
    CODEX_EFFECT_GRADIENT = 5,
    CODEX_EFFECT_SHALLOW_BREATH = 6,
};

struct codex_lighting_zone {
    uint32_t color;
    float brightness;
    float speed;
    float mode;
    enum codex_effect effect;
};

struct codex_agent_lighting {
    struct codex_lighting_zone zone;
    bool sk;
    bool sa;
    uint8_t reserved[2];
};

struct codex_lighting_model {
    struct codex_lighting_zone keys;
    struct codex_lighting_zone ambient;
    struct codex_agent_lighting agents[CODEX_AGENT_COUNT];
};

int codex_lighting_apply_rgbcfg(const uint8_t *params, size_t len);
int codex_lighting_apply_thstatus(const uint8_t *params, size_t len);
struct codex_lighting_model codex_lighting_snapshot(void);

bool codex_lighting_zone_on(const struct codex_lighting_zone *zone);
bool codex_lighting_all_off(const struct codex_lighting_model *model);

/* Task 12 may override this non-blocking hook to schedule renderer work. */
void codex_lighting_changed(void);

#if defined(CONFIG_ZTEST)
void codex_lighting_test_reset(void);
#endif
