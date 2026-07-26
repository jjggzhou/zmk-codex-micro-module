#include <limits.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <codex/renderer.h>
#include <codex/state.h>

static const uint16_t keys_pixels[] = { 8, 0, 6, 2 };
static const uint16_t ambient_pixels[] = { 9, 1 };
static const uint16_t agent0_pixels[] = { 3 };
static const uint16_t agent1_pixels[] = { 4 };
static const uint16_t agent2_pixels[] = { 5 };
static const uint16_t agent3_pixels[] = { 7 };
static const uint16_t agent4_pixels[] = { 10 };
static const uint16_t agent5_pixels[] = { 11 };

static const struct codex_rgb_layout layout = {
    .pixel_count = 12,
    .zones =
        {
            [CODEX_RGB_ZONE_KEYS] = {keys_pixels, ARRAY_SIZE(keys_pixels)},
            [CODEX_RGB_ZONE_AMBIENT] = {ambient_pixels,
                                        ARRAY_SIZE(ambient_pixels)},
            [CODEX_RGB_ZONE_AGENT_0] = {agent0_pixels,
                                        ARRAY_SIZE(agent0_pixels)},
            [CODEX_RGB_ZONE_AGENT_1] = {agent1_pixels,
                                        ARRAY_SIZE(agent1_pixels)},
            [CODEX_RGB_ZONE_AGENT_2] = {agent2_pixels,
                                        ARRAY_SIZE(agent2_pixels)},
            [CODEX_RGB_ZONE_AGENT_3] = {agent3_pixels,
                                        ARRAY_SIZE(agent3_pixels)},
            [CODEX_RGB_ZONE_AGENT_4] = {agent4_pixels,
                                        ARRAY_SIZE(agent4_pixels)},
            [CODEX_RGB_ZONE_AGENT_5] = {agent5_pixels,
                                        ARRAY_SIZE(agent5_pixels)},
        },
};

static struct led_rgb output_pixels[12];
static struct codex_lighting_model last_output_model;
static atomic_t output_count;
static atomic_t output_depth;
static atomic_t max_output_depth;
static bool mutate_during_output;
static bool block_output;
static struct k_sem output_entered;
static struct k_sem output_release;

const struct codex_rgb_layout* codex_renderer_layout(void) { return &layout; }

int codex_renderer_output_frame(
    const struct codex_lighting_model* model, int64_t now_ms, bool output_enabled)
{
    int depth = atomic_inc(&output_depth) + 1;

    if (depth > atomic_get(&max_output_depth)) {
        atomic_set(&max_output_depth, depth);
    }
    last_output_model = *model;
    atomic_inc(&output_count);
    if (block_output) {
        k_sem_give(&output_entered);
        k_sem_take(&output_release, K_FOREVER);
    }
    if (mutate_during_output) {
        static const uint8_t update[] = "{\"keys\":{\"c\":65280}}";

        mutate_during_output = false;
        zassert_ok(codex_lighting_apply_rgbcfg(update, sizeof(update) - 1U));
    }
    int ret = codex_renderer_render_model(
        model, &layout, now_ms, output_enabled, output_pixels, ARRAY_SIZE(output_pixels));
    atomic_dec(&output_depth);
    return ret;
}

static struct codex_lighting_model model_with_keys(
    enum codex_effect effect, uint32_t color, float brightness, float speed)
{
    struct codex_lighting_model model = { 0 };

    model.keys = (struct codex_lighting_zone) {
        .effect = effect,
        .color = color,
        .brightness = brightness,
        .speed = speed,
    };
    return model;
}

static void assert_rgb(struct led_rgb actual, uint8_t r, uint8_t g, uint8_t b)
{
    zassert_equal(actual.r, r, "got %u,%u,%u want %u,%u,%u", actual.r, actual.g, actual.b, r, g, b);
    zassert_equal(actual.g, g, "got %u,%u,%u want %u,%u,%u", actual.r, actual.g, actual.b, r, g, b);
    zassert_equal(actual.b, b, "got %u,%u,%u want %u,%u,%u", actual.r, actual.g, actual.b, r, g, b);
}

static void assert_black(const struct led_rgb* pixels, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        assert_rgb(pixels[i], 0, 0, 0);
    }
}

static void before_renderer(void* fixture)
{
    ARG_UNUSED(fixture);
    codex_lighting_test_reset();
    codex_renderer_test_reset();
    codex_renderer_test_set_auto_off(true);
    memset(output_pixels, 0xA5, sizeof(output_pixels));
    memset(&last_output_model, 0, sizeof(last_output_model));
    atomic_clear(&output_count);
    atomic_clear(&output_depth);
    atomic_clear(&max_output_depth);
    mutate_during_output = false;
    block_output = false;
    k_sem_init(&output_entered, 0, 1);
    k_sem_init(&output_release, 0, 1);
}

ZTEST(renderer, test_off_solid_zero_brightness_and_brightness_once)
{
    struct led_rgb pixels[12];
    struct codex_lighting_model model = model_with_keys(CODEX_EFFECT_OFF, 0x804020, 1.0f, 0.0f);

    memset(pixels, 0xA5, sizeof(pixels));
    zassert_ok(
        codex_renderer_render_model(&model, &layout, INT64_MIN, true, pixels, ARRAY_SIZE(pixels)));
    assert_black(pixels, ARRAY_SIZE(pixels));

    model.keys.effect = CODEX_EFFECT_SOLID;
    model.keys.brightness = 0.5f;
    zassert_ok(
        codex_renderer_render_model(&model, &layout, INT64_MAX, true, pixels, ARRAY_SIZE(pixels)));
    for (size_t i = 0; i < ARRAY_SIZE(keys_pixels); i++) {
        assert_rgb(pixels[keys_pixels[i]], 64, 32, 16);
    }
    model.keys.brightness = 0.0f;
    zassert_ok(codex_renderer_render_model(&model, &layout, 0, true, pixels, ARRAY_SIZE(pixels)));
    assert_black(pixels, ARRAY_SIZE(pixels));
}

ZTEST(renderer, test_snake_golden_frames_include_speed_zero_and_boundary)
{
    struct led_rgb pixels[12];
    struct codex_lighting_model model = model_with_keys(CODEX_EFFECT_SNAKE, 0x804020, 1.0f, 0.0f);

    zassert_ok(
        codex_renderer_render_model(&model, &layout, INT64_MAX, true, pixels, ARRAY_SIZE(pixels)));
    assert_rgb(pixels[8], 128, 64, 32);
    assert_rgb(pixels[0], 0, 0, 0);
    assert_rgb(pixels[6], 32, 16, 8);
    assert_rgb(pixels[2], 64, 32, 16);

    model.keys.speed = 1.0f;
    zassert_ok(codex_renderer_render_model(&model, &layout, 500, true, pixels, ARRAY_SIZE(pixels)));
    assert_rgb(pixels[8], 32, 16, 8);
    assert_rgb(pixels[0], 64, 32, 16);
    assert_rgb(pixels[6], 128, 64, 32);
    assert_rgb(pixels[2], 0, 0, 0);
}

ZTEST(renderer, test_rainbow_breath_gradient_and_shallow_breath_golden_frames)
{
    struct led_rgb pixels[12];
    struct codex_lighting_model model = model_with_keys(CODEX_EFFECT_RAINBOW, 0, 1.0f, 0.0f);

    zassert_ok(codex_renderer_render_model(&model, &layout, 0, true, pixels, ARRAY_SIZE(pixels)));
    assert_rgb(pixels[8], 255, 0, 0);
    assert_rgb(pixels[0], 63, 192, 0);
    assert_rgb(pixels[6], 0, 126, 129);
    assert_rgb(pixels[2], 66, 0, 189);
    model.keys.speed = 1.0f;
    zassert_ok(codex_renderer_render_model(&model, &layout, 250, true, pixels, ARRAY_SIZE(pixels)));
    assert_rgb(pixels[8], 63, 192, 0);
    assert_rgb(pixels[0], 0, 126, 129);
    assert_rgb(pixels[6], 66, 0, 189);
    assert_rgb(pixels[2], 255, 0, 0);

    model = model_with_keys(CODEX_EFFECT_BREATH, 0x804020, 1.0f, 1.0f);
    zassert_ok(codex_renderer_render_model(&model, &layout, 0, true, pixels, ARRAY_SIZE(pixels)));
    assert_black(pixels, ARRAY_SIZE(pixels));
    zassert_ok(codex_renderer_render_model(&model, &layout, 500, true, pixels, ARRAY_SIZE(pixels)));
    assert_rgb(pixels[8], 128, 64, 32);
    model.keys.speed = 0.0f;
    zassert_ok(
        codex_renderer_render_model(&model, &layout, INT64_MIN, true, pixels, ARRAY_SIZE(pixels)));
    assert_black(pixels, ARRAY_SIZE(pixels));

    model = model_with_keys(CODEX_EFFECT_GRADIENT, 0x804020, 1.0f, 0.0f);
    zassert_ok(codex_renderer_render_model(&model, &layout, 99, true, pixels, ARRAY_SIZE(pixels)));
    assert_rgb(pixels[8], 0, 0, 0);
    assert_rgb(pixels[0], 43, 21, 11);
    assert_rgb(pixels[6], 85, 43, 21);
    assert_rgb(pixels[2], 128, 64, 32);
    model.keys.speed = 1.0f;
    zassert_ok(codex_renderer_render_model(&model, &layout, 500, true, pixels, ARRAY_SIZE(pixels)));
    assert_rgb(pixels[8], 64, 32, 16);
    assert_rgb(pixels[0], 107, 53, 27);
    assert_rgb(pixels[6], 21, 11, 5);
    assert_rgb(pixels[2], 64, 32, 16);

    model = model_with_keys(CODEX_EFFECT_SHALLOW_BREATH, 0x804020, 1.0f, 1.0f);
    zassert_ok(codex_renderer_render_model(&model, &layout, 0, true, pixels, ARRAY_SIZE(pixels)));
    assert_rgb(pixels[8], 80, 40, 20);
    zassert_ok(codex_renderer_render_model(&model, &layout, 500, true, pixels, ARRAY_SIZE(pixels)));
    assert_rgb(pixels[8], 128, 64, 32);
    model.keys.speed = 0.0f;
    zassert_ok(
        codex_renderer_render_model(&model, &layout, INT64_MAX, true, pixels, ARRAY_SIZE(pixels)));
    assert_rgb(pixels[8], 80, 40, 20);
}

ZTEST(renderer, test_all_eight_noncontiguous_zones_are_independent)
{
    struct codex_lighting_model model = { 0 };
    struct led_rgb pixels[12];

    model.keys = (struct codex_lighting_zone) {
        .effect = CODEX_EFFECT_SOLID, .color = 0x110000, .brightness = 1
    };
    model.ambient = (struct codex_lighting_zone) {
        .effect = CODEX_EFFECT_SOLID, .color = 0x002200, .brightness = 1
    };
    for (size_t i = 0; i < CODEX_AGENT_COUNT; i++) {
        model.agents[i].zone = (struct codex_lighting_zone) {
            .effect = CODEX_EFFECT_SOLID,
            .color = (uint32_t)(0x33U + i),
            .brightness = 1,
        };
    }
    zassert_ok(codex_renderer_render_model(&model, &layout, 0, true, pixels, ARRAY_SIZE(pixels)));
    assert_rgb(pixels[8], 0x11, 0, 0);
    assert_rgb(pixels[9], 0, 0x22, 0);
    const uint16_t agent_pixels[] = { 3, 4, 5, 7, 10, 11 };
    for (size_t i = 0; i < CODEX_AGENT_COUNT; i++) {
        assert_rgb(pixels[agent_pixels[i]], 0, 0, 0x33 + i);
    }
}

ZTEST(renderer, test_empty_short_duplicate_and_invalid_model_fail_closed_with_sentinels)
{
    struct guarded {
        struct led_rgb pre;
        struct led_rgb pixels[12];
        struct led_rgb post;
    } guarded;
    struct codex_lighting_model model = model_with_keys(CODEX_EFFECT_SOLID, 0xFFFFFF, 1.0f, 0.0f);
    struct codex_rgb_layout bad = layout;
    const struct led_rgb sentinel = { .r = 0xA5, .g = 0xA5, .b = 0xA5 };

    memset(&guarded, 0xA5, sizeof(guarded));
    bad.zones[0] = (struct codex_rgb_zone_map) { NULL, 0 };
    zassert_equal(codex_renderer_render_model(&model, &bad, 0, true, guarded.pixels, 12), -EINVAL);
    assert_black(guarded.pixels, 12);
    zassert_mem_equal(&guarded.pre, &sentinel, sizeof(guarded.pre));
    zassert_mem_equal(&guarded.post, &sentinel, sizeof(guarded.post));

    bad = layout;
    bad.zones[7] = (struct codex_rgb_zone_map) { agent0_pixels, 1 };
    zassert_equal(codex_renderer_render_model(&model, &bad, 0, true, guarded.pixels, 12), -EINVAL);
    bad = layout;
    zassert_equal(codex_renderer_render_model(&model, &bad, 0, true, guarded.pixels, 4), -EINVAL);
    assert_black(guarded.pixels, 4);

    model.keys.effect = (enum codex_effect)99;
    zassert_equal(
        codex_renderer_render_model(&model, &layout, 0, true, guarded.pixels, 12), -EINVAL);
    assert_black(guarded.pixels, 12);
    model.keys.effect = CODEX_EFFECT_SOLID;
    model.keys.brightness = NAN;
    zassert_equal(
        codex_renderer_render_model(&model, &layout, 0, true, guarded.pixels, 12), -EINVAL);
    assert_black(guarded.pixels, 12);
}

ZTEST(renderer, test_idle_gate_preserves_target_wake_partial_off_and_auto_off_disabled)
{
    static const uint8_t on[] = "{\"keys\":{\"e\":1,\"b\":1,\"c\":16711680},"
                                "\"ambient\":{\"e\":1,\"b\":1,\"c\":255}}";
    static const uint8_t keys_off[] = "{\"keys\":{\"e\":0}}";
    static const uint8_t ambient_off[] = "{\"ambient\":{\"e\":0}}";
    static const uint8_t agent_on[] = "[{\"id\":0,\"e\":1,\"b\":1,\"c\":16777215}]";
    static const uint8_t agent_off[] = "[{\"id\":0,\"e\":0}]";

    zassert_ok(codex_lighting_apply_rgbcfg(on, sizeof(on) - 1));
    zassert_ok(codex_lighting_apply_thstatus(agent_on, sizeof(agent_on) - 1));
    codex_renderer_test_drain();
    codex_renderer_on_activity(ZMK_ACTIVITY_IDLE);
    codex_renderer_test_drain();
    assert_black(output_pixels, ARRAY_SIZE(output_pixels));
    zassert_equal(codex_lighting_snapshot().keys.effect, CODEX_EFFECT_SOLID);
    zassert_ok(codex_lighting_apply_rgbcfg(keys_off, sizeof(keys_off) - 1));
    zassert_ok(codex_lighting_apply_thstatus(agent_off, sizeof(agent_off) - 1));
    codex_renderer_test_drain();
    /* Lighting RPC schedules output but is not user activity and cannot wake
     * the idle gate. */
    assert_black(output_pixels, ARRAY_SIZE(output_pixels));
    codex_renderer_on_activity(ZMK_ACTIVITY_ACTIVE);
    codex_renderer_test_drain();
    assert_rgb(output_pixels[8], 0, 0, 0);
    assert_rgb(output_pixels[9], 0, 0, 255);
    assert_rgb(output_pixels[3], 0, 0, 0);

    zassert_ok(codex_lighting_apply_rgbcfg(ambient_off, sizeof(ambient_off) - 1));
    codex_renderer_test_drain();
    codex_renderer_on_activity(ZMK_ACTIVITY_IDLE);
    codex_renderer_on_activity(ZMK_ACTIVITY_ACTIVE);
    codex_renderer_test_drain();
    assert_black(output_pixels, ARRAY_SIZE(output_pixels));

    codex_renderer_test_set_auto_off(false);
    zassert_ok(codex_lighting_apply_rgbcfg(on, sizeof(on) - 1));
    codex_renderer_test_drain();
    codex_renderer_on_activity(ZMK_ACTIVITY_IDLE);
    codex_renderer_test_drain();
    assert_rgb(output_pixels[9], 0, 0, 255);
}

ZTEST(renderer,
    test_frame_is_fresh_indicator_untouched_and_model_change_during_output_is_not_recursive)
{
    static const uint8_t on[] = "{\"keys\":{\"e\":1,\"b\":1,\"c\":16711680}}";
    uint8_t indicator_before = codex_indicator_bits();

    zassert_ok(codex_lighting_apply_rgbcfg(on, sizeof(on) - 1));
    mutate_during_output = true;
    codex_renderer_test_drain();
    codex_renderer_test_drain();
    zassert_equal(last_output_model.keys.color, 0x00FF00);
    zassert_equal(atomic_get(&max_output_depth), 1);
    zassert_equal(codex_indicator_bits(), indicator_before);
}

ZTEST(renderer, test_callback_storm_coalesces_without_recursion_or_exhaustion)
{
    static const uint8_t on[] = "{\"keys\":{\"e\":1,\"b\":1,\"c\":1}}";

    block_output = true;
    zassert_ok(codex_lighting_apply_rgbcfg(on, sizeof(on) - 1));
    zassert_ok(k_sem_take(&output_entered, K_SECONDS(1)));
    for (size_t i = 0; i < 1000; i++) {
        codex_lighting_changed();
    }
    block_output = false;
    k_sem_give(&output_release);
    codex_renderer_test_drain();
    zassert_true(atomic_get(&output_count) <= 2);
    zassert_equal(atomic_get(&max_output_depth), 1);
}

struct frame_concurrency_ctx {
    struct k_sem start;
    struct k_sem done;
    atomic_t failures;
};

K_THREAD_STACK_DEFINE(frame_reader_a_stack, 2048);
K_THREAD_STACK_DEFINE(frame_reader_b_stack, 2048);
static struct k_thread frame_reader_a_thread;
static struct k_thread frame_reader_b_thread;

static void frame_reader(void* arg1, void* arg2, void* arg3)
{
    struct frame_concurrency_ctx* ctx = arg1;
    struct led_rgb pixels[12];

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    k_sem_take(&ctx->start, K_FOREVER);
    for (size_t i = 0; i < 1000; i++) {
        codex_renderer_frame((int64_t)i, pixels, ARRAY_SIZE(pixels));
        bool red = pixels[8].r == 255U && pixels[8].g == 0U && pixels[8].b == 0U;
        bool green = pixels[8].r == 0U && pixels[8].g == 255U && pixels[8].b == 0U;

        if ((!red && !green) || pixels[8].r != pixels[9].r || pixels[8].g != pixels[9].g
            || pixels[8].b != pixels[9].b) {
            atomic_inc(&ctx->failures);
        }
    }
    k_sem_give(&ctx->done);
}

ZTEST(renderer, test_concurrent_frames_use_complete_snapshots_while_model_changes)
{
    static const char* const states[] = {
        "{\"keys\":{\"e\":1,\"b\":1,\"c\":16711680},"
        "\"ambient\":{\"e\":1,\"b\":1,\"c\":16711680}}",
        "{\"keys\":{\"e\":1,\"b\":1,\"c\":65280},"
        "\"ambient\":{\"e\":1,\"b\":1,\"c\":65280}}",
    };
    struct frame_concurrency_ctx ctx;

    k_sem_init(&ctx.start, 0, 2);
    k_sem_init(&ctx.done, 0, 2);
    atomic_clear(&ctx.failures);
    zassert_ok(codex_lighting_apply_rgbcfg((const uint8_t*)states[0], strlen(states[0])));
    k_thread_create(&frame_reader_a_thread, frame_reader_a_stack,
        K_THREAD_STACK_SIZEOF(frame_reader_a_stack), frame_reader, &ctx, NULL, NULL,
        K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
    k_thread_create(&frame_reader_b_thread, frame_reader_b_stack,
        K_THREAD_STACK_SIZEOF(frame_reader_b_stack), frame_reader, &ctx, NULL, NULL,
        K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
    k_sem_give(&ctx.start);
    k_sem_give(&ctx.start);
    for (size_t i = 0; i < 500; i++) {
        zassert_ok(
            codex_lighting_apply_rgbcfg((const uint8_t*)states[i & 1U], strlen(states[i & 1U])));
        k_yield();
    }
    zassert_ok(k_sem_take(&ctx.done, K_SECONDS(2)));
    zassert_ok(k_sem_take(&ctx.done, K_SECONDS(2)));
    zassert_equal(atomic_get(&ctx.failures), 0);
}

ZTEST_SUITE(renderer, NULL, NULL, before_renderer, NULL, NULL);
