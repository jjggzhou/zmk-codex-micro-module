#include <codex/renderer.h>

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#if !defined(CONFIG_ZTEST)
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#endif

#define CODEX_RENDER_FRAME_MS 16

/* Periods, tail length, and wave shapes are bounded estimates. They are not
 * claimed as frame-exact original-hardware curves pending measurement. */

static atomic_t activity_is_active = ATOMIC_INIT(1);

#if defined(CONFIG_ZTEST)
static bool test_auto_off = IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE);
__weak void codex_renderer_test_changed_hook(void) { }
#endif

static bool auto_off_enabled(void)
{
#if defined(CONFIG_ZTEST)
    return test_auto_off;
#else
    return IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE);
#endif
}

static bool valid_unit(float value) { return value >= 0.0f && value <= 1.0f; }

static uint8_t unit_byte(float value) { return (uint8_t)(value * 255.0f + 0.5f); }

static uint8_t animation_phase(int64_t now_ms, float speed)
{
    uint8_t speed_byte = unit_byte(speed);

    if (speed_byte == 0U) {
        return 0U;
    }
    uint32_t period_ms = 4000U - (3000U * speed_byte + 127U) / 255U;
    uint64_t wrapped_ms = (uint64_t)now_ms % period_ms;

    return (uint8_t)((wrapped_ms * 256U) / period_ms);
}

static uint8_t triangle(uint8_t phase)
{
    if (phase < 128U) {
        return (uint8_t)(phase * 2U);
    }
    return (uint8_t)MIN(255U, (256U - phase) * 2U);
}

static uint8_t scale_channel(uint8_t channel, uint8_t level, uint8_t brightness)
{
    uint32_t effect_value = ((uint32_t)channel * level + 127U) / 255U;

    return (uint8_t)((effect_value * brightness + 127U) / 255U);
}

static struct led_rgb scaled_color(uint32_t packed, uint8_t level, uint8_t brightness)
{
    return (struct led_rgb) {
        .r = scale_channel((packed >> 16U) & 0xFFU, level, brightness),
        .g = scale_channel((packed >> 8U) & 0xFFU, level, brightness),
        .b = scale_channel(packed & 0xFFU, level, brightness),
    };
}

static struct led_rgb rainbow_color(uint8_t hue, uint8_t brightness)
{
    struct led_rgb color;

    if (hue < 85U) {
        color = (struct led_rgb) { .r = 255U - hue * 3U, .g = hue * 3U };
    } else if (hue < 170U) {
        hue -= 85U;
        color = (struct led_rgb) { .g = 255U - hue * 3U, .b = hue * 3U };
    } else {
        hue -= 170U;
        color = (struct led_rgb) { .r = hue * 3U, .b = 255U - hue * 3U };
    }
    color.r = scale_channel(color.r, 255U, brightness);
    color.g = scale_channel(color.g, 255U, brightness);
    color.b = scale_channel(color.b, 255U, brightness);
    return color;
}

static const struct codex_lighting_zone* zone_for_id(
    const struct codex_lighting_model* model, size_t id)
{
    if (id == CODEX_RGB_ZONE_KEYS) {
        return &model->keys;
    }
    if (id == CODEX_RGB_ZONE_AMBIENT) {
        return &model->ambient;
    }
    return &model->agents[id - CODEX_RGB_ZONE_AGENT_0].zone;
}

static bool valid_model(const struct codex_lighting_model* model)
{
    for (size_t id = 0U; id < CODEX_RGB_ZONE_COUNT; id++) {
        const struct codex_lighting_zone* zone = zone_for_id(model, id);

        if ((unsigned int)zone->effect > CODEX_EFFECT_SHALLOW_BREATH || zone->color > 0xFFFFFFU
            || !valid_unit(zone->brightness) || !valid_unit(zone->speed)
            || !valid_unit(zone->mode)) {
            return false;
        }
    }
    return true;
}

static bool valid_layout(const struct codex_rgb_layout* layout, size_t count)
{
    if (layout == NULL || layout->pixel_count != count || count == 0U
        || count > CODEX_RENDERER_MAX_PIXELS) {
        return false;
    }
    for (size_t zone = 0U; zone < CODEX_RGB_ZONE_COUNT; zone++) {
        const struct codex_rgb_zone_map* map = &layout->zones[zone];

        if (map->pixels == NULL || map->count == 0U || map->count > count) {
            return false;
        }
        for (size_t item = 0U; item < map->count; item++) {
            uint16_t pixel = map->pixels[item];

            if (pixel >= count) {
                return false;
            }
            for (size_t prior_zone = 0U; prior_zone <= zone; prior_zone++) {
                const struct codex_rgb_zone_map* prior = &layout->zones[prior_zone];
                size_t limit = prior_zone == zone ? item : prior->count;

                for (size_t prior_item = 0U; prior_item < limit; prior_item++) {
                    if (prior->pixels[prior_item] == pixel) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

static struct led_rgb render_pixel(
    const struct codex_lighting_zone* zone, int64_t now_ms, size_t position, size_t zone_count)
{
    uint8_t brightness = unit_byte(zone->brightness);
    uint8_t phase = animation_phase(now_ms, zone->speed);
    uint8_t level = 255U;

    switch (zone->effect) {
    case CODEX_EFFECT_OFF:
        return (struct led_rgb) { 0 };
    case CODEX_EFFECT_SOLID:
        break;
    case CODEX_EFFECT_SNAKE: {
        size_t head = ((size_t)phase * zone_count) >> 8U;
        size_t distance = (head + zone_count - position) % zone_count;

        level = distance == 0U ? 255U : distance == 1U ? 128U : distance == 2U ? 64U : 0U;
        break;
    }
    case CODEX_EFFECT_RAINBOW: {
        uint8_t hue = phase + (uint8_t)((position * 256U) / zone_count);

        return rainbow_color(hue, brightness);
    }
    case CODEX_EFFECT_BREATH:
        level = triangle(phase);
        break;
    case CODEX_EFFECT_GRADIENT:
        level = zone_count == 1U ? 255U : (uint8_t)(phase + (position * 255U) / (zone_count - 1U));
        break;
    case CODEX_EFFECT_SHALLOW_BREATH:
        level = (uint8_t)(160U + ((uint16_t)triangle(phase) * 95U) / 255U);
        break;
    }
    return scaled_color(zone->color, level, brightness);
}

int codex_renderer_render_model(const struct codex_lighting_model* model,
    const struct codex_rgb_layout* layout, int64_t now_ms, bool output_enabled,
    struct led_rgb* pixels, size_t count)
{
    if (pixels == NULL || count == 0U || count > CODEX_RENDERER_MAX_PIXELS) {
        return -EINVAL;
    }
    memset(pixels, 0, count * sizeof(*pixels));
    if (model == NULL || !valid_model(model) || !valid_layout(layout, count)) {
        return -EINVAL;
    }
    if (!output_enabled) {
        return 0;
    }
    for (size_t id = 0U; id < CODEX_RGB_ZONE_COUNT; id++) {
        const struct codex_lighting_zone* zone = zone_for_id(model, id);
        const struct codex_rgb_zone_map* map = &layout->zones[id];

        for (size_t position = 0U; position < map->count; position++) {
            pixels[map->pixels[position]] = render_pixel(zone, now_ms, position, map->count);
        }
    }
    return 0;
}

__weak const struct codex_rgb_layout* codex_renderer_layout(void) { return NULL; }

__weak int codex_renderer_output_frame(
    const struct codex_lighting_model* model, int64_t now_ms, bool output_enabled)
{
    ARG_UNUSED(model);
    ARG_UNUSED(now_ms);
    ARG_UNUSED(output_enabled);
    return -ENODEV;
}

void codex_renderer_frame(int64_t now_ms, struct led_rgb* pixels, size_t count)
{
    struct codex_lighting_model model = codex_lighting_snapshot();
    bool enabled = !auto_off_enabled() || atomic_get(&activity_is_active) != 0;

    (void)codex_renderer_render_model(
        &model, codex_renderer_layout(), now_ms, enabled, pixels, count);
}

static bool model_is_animated(const struct codex_lighting_model* model)
{
    for (size_t id = 0U; id < CODEX_RGB_ZONE_COUNT; id++) {
        enum codex_effect effect = zone_for_id(model, id)->effect;

        if (effect == CODEX_EFFECT_SNAKE || effect == CODEX_EFFECT_RAINBOW
            || effect == CODEX_EFFECT_BREATH || effect == CODEX_EFFECT_GRADIENT
            || effect == CODEX_EFFECT_SHALLOW_BREATH) {
            return true;
        }
    }
    return false;
}

static void render_work_handler(struct k_work* work);
static K_WORK_DELAYABLE_DEFINE(render_work, render_work_handler);

static void render_work_handler(struct k_work* work)
{
    ARG_UNUSED(work);
    struct codex_lighting_model model = codex_lighting_snapshot();
    bool enabled = !auto_off_enabled() || atomic_get(&activity_is_active) != 0;
    int ret = codex_renderer_output_frame(&model, k_uptime_get(), enabled);

    if (ret == 0 && enabled && model_is_animated(&model)) {
        (void)k_work_reschedule(&render_work, K_MSEC(CODEX_RENDER_FRAME_MS));
    }
}

void codex_lighting_changed(void)
{
#if defined(CONFIG_ZTEST)
    codex_renderer_test_changed_hook();
#endif
    (void)k_work_reschedule(&render_work, K_NO_WAIT);
}

void codex_renderer_on_activity(enum zmk_activity_state state)
{
    if (auto_off_enabled()) {
        atomic_set(&activity_is_active, state == ZMK_ACTIVITY_ACTIVE);
    }
    (void)k_work_reschedule(&render_work, K_NO_WAIT);
}

#if !defined(CONFIG_ZTEST)
static int renderer_activity_listener(const zmk_event_t* event)
{
    const struct zmk_activity_state_changed* changed = as_zmk_activity_state_changed(event);

    if (changed == NULL) {
        return -ENOTSUP;
    }
    codex_renderer_on_activity(changed->state);
    return 0;
}

ZMK_LISTENER(codex_renderer, renderer_activity_listener);
ZMK_SUBSCRIPTION(codex_renderer, zmk_activity_state_changed);
#endif

#if defined(CONFIG_ZTEST)
void codex_renderer_test_reset(void)
{
    struct k_work_sync sync;

    (void)k_work_cancel_delayable_sync(&render_work, &sync);
    atomic_set(&activity_is_active, 1);
    test_auto_off = IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE);
}

void codex_renderer_test_drain(void)
{
    struct k_work_sync sync;

    (void)k_work_flush_delayable(&render_work, &sync);
}

void codex_renderer_test_set_auto_off(bool enabled) { test_auto_off = enabled; }
#endif
