#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <codex/lighting.h>
#include <codex/framing.h>

static size_t changed_count;
static bool commit_barrier_enabled;
static atomic_t commit_barrier_arrivals;
static struct k_sem commit_barrier_release;

void codex_lighting_changed(void)
{
    changed_count++;
}

void codex_lighting_test_before_commit(void)
{
    if (!commit_barrier_enabled) {
        return;
    }
    if (atomic_inc(&commit_barrier_arrivals) == 1) {
        k_sem_give(&commit_barrier_release);
        k_sem_give(&commit_barrier_release);
    }
    k_sem_take(&commit_barrier_release, K_FOREVER);
}

static int rgbcfg(const char *json)
{
    return codex_lighting_apply_rgbcfg((const uint8_t *)json, strlen(json));
}

static int thstatus(const char *json)
{
    return codex_lighting_apply_thstatus((const uint8_t *)json, strlen(json));
}

static void assert_zone(const struct codex_lighting_zone *zone,
                        enum codex_effect effect, float brightness, float speed,
                        uint32_t color, float mode)
{
    zassert_equal(zone->effect, effect);
    zassert_within(zone->brightness, brightness, 0.0001f);
    zassert_within(zone->speed, speed, 0.0001f);
    zassert_equal(zone->color, color);
    zassert_within(zone->mode, mode, 0.0001f);
}

static void before(void *fixture)
{
    ARG_UNUSED(fixture);
    codex_lighting_test_reset();
    changed_count = 0U;
    commit_barrier_enabled = false;
}

ZTEST(lighting, test_defaults_are_all_off_and_helpers_cover_only_rgb_model)
{
    struct codex_lighting_model model = codex_lighting_snapshot();

    assert_zone(&model.keys, CODEX_EFFECT_OFF, 0.0f, 0.0f, 0U, 0.0f);
    assert_zone(&model.ambient, CODEX_EFFECT_OFF, 0.0f, 0.0f, 0U, 0.0f);
    for (size_t i = 0U; i < CODEX_AGENT_COUNT; i++) {
        assert_zone(&model.agents[i].zone, CODEX_EFFECT_OFF, 0.0f, 0.0f, 0U,
                    0.0f);
        zassert_false(model.agents[i].sk);
        zassert_false(model.agents[i].sa);
    }
    zassert_true(codex_lighting_all_off(&model));
    zassert_false(codex_lighting_zone_on(&model.keys));
}

ZTEST(lighting, test_public_apply_seams_accept_outer_space_and_enforce_rpc_bound)
{
    uint8_t oversized[CODEX_JSON_MAX_SIZE + 1U];

    zassert_ok(rgbcfg(" \n {\"keys\":{\"e\":1}} \t"));
    zassert_equal(codex_lighting_snapshot().keys.effect, CODEX_EFFECT_SOLID);
    memset(oversized, ' ', sizeof(oversized));
    zassert_equal(codex_lighting_apply_rgbcfg(oversized, sizeof(oversized)),
                  -EMSGSIZE);
    zassert_equal(codex_lighting_apply_thstatus(oversized, sizeof(oversized)),
                  -EMSGSIZE);
}

ZTEST(lighting, test_partial_zone_updates_merge_and_all_fields_parse)
{
    struct codex_lighting_model model;

    zassert_ok(rgbcfg("{\"keys\":{\"e\":1,\"b\":0.25,\"s\":5e-1,"
                      "\"c\":1122867,\"m\":1}}"));
    model = codex_lighting_snapshot();
    assert_zone(&model.keys, CODEX_EFFECT_SOLID, 0.25f, 0.5f, 0x112233U, 1.0f);
    assert_zone(&model.ambient, CODEX_EFFECT_OFF, 0.0f, 0.0f, 0U, 0.0f);

    zassert_ok(rgbcfg("{\"ambient\":{\"e\":3,\"c\":16711935},"
                      "\"keys\":{\"b\":0.75}}"));
    model = codex_lighting_snapshot();
    assert_zone(&model.keys, CODEX_EFFECT_SOLID, 0.75f, 0.5f, 0x112233U, 1.0f);
    assert_zone(&model.ambient, CODEX_EFFECT_RAINBOW, 0.0f, 0.0f,
                0xFF00FFU, 0.0f);
    zassert_equal(changed_count, 2U);
}

ZTEST(lighting, test_numeric_ranges_clamp_and_effect_boundaries_validate)
{
    struct codex_lighting_model model;

    zassert_ok(rgbcfg("{\"keys\":{\"e\":6,\"b\":-2e3,\"s\":7.5,"
                      "\"c\":99999999,\"m\":-1}}"));
    model = codex_lighting_snapshot();
    assert_zone(&model.keys, CODEX_EFFECT_SHALLOW_BREATH, 0.0f, 1.0f,
                0xFFFFFFU, 0.0f);
    zassert_ok(rgbcfg("{\"keys\":{\"e\":0,\"c\":-5}}"));
    model = codex_lighting_snapshot();
    zassert_equal(model.keys.effect, CODEX_EFFECT_OFF);
    zassert_equal(model.keys.color, 0U);
    zassert_ok(rgbcfg("{\"keys\":{\"e\":1e0,\"c\":1.6777215e7}}"));
    model = codex_lighting_snapshot();
    zassert_equal(model.keys.effect, CODEX_EFFECT_SOLID);
    zassert_equal(model.keys.color, 0xFFFFFFU);
    zassert_ok(rgbcfg("{\"keys\":{\"e\":0.000000000000000000000000000001e30,"
                      "\"b\":0e99999,\"c\":1.5e1}}"));
    model = codex_lighting_snapshot();
    zassert_equal(model.keys.effect, CODEX_EFFECT_SOLID);
    zassert_within(model.keys.brightness, 0.0f, 0.0001f);
    zassert_equal(model.keys.color, 15U);

    struct codex_lighting_model before = model;
    static const char *const invalid[] = {
        "{\"keys\":{\"e\":7}}", "{\"keys\":{\"e\":-1}}",
        "{\"keys\":{\"e\":1.5}}", "{\"keys\":{\"c\":1.5}}",
        "{\"keys\":{\"e\":1e-401}}",
        "{\"keys\":{\"c\":1e-401}}",
        "{\"keys\":{\"c\":16777215.5}}",
        "{\"keys\":{\"c\":-16777216.5}}",
        "{\"keys\":{\"c\":1e1000}}", "{\"keys\":{\"b\":1e1000}}",
    };

    for (size_t i = 0U; i < ARRAY_SIZE(invalid); i++) {
        struct codex_lighting_model after;

        zassert_equal(rgbcfg(invalid[i]), -EINVAL, "%s", invalid[i]);
        after = codex_lighting_snapshot();
        zassert_mem_equal(&after, &before, sizeof(before));
    }
    zassert_equal(changed_count, 4U);
}

ZTEST(lighting, test_color_uses_exact_decimal_exponent_value_before_clamp)
{
    static const char prefix[] = "{\"keys\":{\"c\":0.";
    static const char suffix[] = "1e310}}";
    char request[sizeof(prefix) + 309U + sizeof(suffix)];
    size_t pos = 0U;

    memcpy(&request[pos], prefix, sizeof(prefix) - 1U);
    pos += sizeof(prefix) - 1U;
    memset(&request[pos], '0', 309U);
    pos += 309U;
    memcpy(&request[pos], suffix, sizeof(suffix));
    zassert_ok(rgbcfg(request));
    zassert_equal(codex_lighting_snapshot().keys.color, 1U);
}

ZTEST(lighting, test_rgbcfg_invalid_or_duplicate_content_rolls_back_whole_command)
{
    zassert_ok(rgbcfg("{\"keys\":{\"e\":1,\"c\":1},"
                      "\"ambient\":{\"e\":1,\"c\":2}}"));
    struct codex_lighting_model before = codex_lighting_snapshot();
    static const char *const invalid[] = {
        "{}", "null", "[]", "{\"keys\":{}}",
        "{\"keys\":{\"e\":2,\"e\":3}}",
        "{\"keys\":{\"e\":2,\"\\u0065\":3}}",
        "{\"keys\":{\"e\":2},\"keys\":{\"e\":3}}",
        "{\"keys\":{\"e\":2},\"\\u006beys\":{\"e\":3}}",
        "{\"keys\":{\"e\":2},\"ambient\":{\"unknown\":1}}",
        "{\"keys\":{\"e\":2},\"ambient\":7}",
        "{\"keys\":{\"e\":2},\"unknown\":{}}",
        "{\"keys\":{\"e\":true}}",
    };

    for (size_t i = 0U; i < ARRAY_SIZE(invalid); i++) {
        struct codex_lighting_model after;

        zassert_equal(rgbcfg(invalid[i]), -EINVAL, "%s", invalid[i]);
        after = codex_lighting_snapshot();
        zassert_mem_equal(&after, &before, sizeof(before), "%s", invalid[i]);
    }
    zassert_equal(changed_count, 1U);
}

ZTEST(lighting, test_agent_subset_updates_merge_and_duplicate_ids_roll_back)
{
    struct codex_lighting_model model;

    zassert_ok(thstatus("[{\"id\":5,\"e\":2,\"b\":0.4,\"s\":0.6,"
                        "\"c\":1193046,\"sk\":1,\"sa\":0},"
                        "{\"id\":1,\"e\":6,\"c\":16777215}]"));
    model = codex_lighting_snapshot();
    assert_zone(&model.agents[5].zone, CODEX_EFFECT_SNAKE, 0.4f, 0.6f,
                0x123456U, 0.0f);
    zassert_true(model.agents[5].sk);
    zassert_false(model.agents[5].sa);
    zassert_equal(model.agents[1].zone.effect, CODEX_EFFECT_SHALLOW_BREATH);
    zassert_equal(model.agents[0].zone.effect, CODEX_EFFECT_OFF);

    zassert_ok(thstatus("[{\"id\":5,\"sa\":1}]"));
    model = codex_lighting_snapshot();
    zassert_equal(model.agents[5].zone.color, 0x123456U);
    zassert_true(model.agents[5].sk);
    zassert_true(model.agents[5].sa);
    zassert_ok(thstatus("[{\"id\":5e0,\"b\":9,\"s\":-3,\"c\":1e20}]"));
    model = codex_lighting_snapshot();
    zassert_within(model.agents[5].zone.brightness, 1.0f, 0.0001f);
    zassert_within(model.agents[5].zone.speed, 0.0f, 0.0001f);
    zassert_equal(model.agents[5].zone.color, 0xFFFFFFU);

    struct codex_lighting_model before = model;
    static const char *const invalid[] = {
        "[]", "{}", "null", "[{\"id\":5}]", "[{\"e\":1}]",
        "[{\"id\":6,\"e\":1}]", "[{\"id\":-1,\"e\":1}]",
        "[{\"id\":1.5,\"e\":1}]",
        "[{\"id\":1e-401,\"e\":1}]",
        "[{\"id\":1,\"sk\":1e-401}]",
        "[{\"id\":1,\"sk\":true}]", "[{\"id\":1,\"sa\":2}]",
        "[{\"id\":1,\"sk\":0,\"sk\":1}]",
        "[{\"id\":1,\"e\":1},{\"id\":1,\"e\":2}]",
        "[{\"id\":1,\"e\":1},{\"id\":2,\"e\":2,\"e\":3}]",
        "[{\"id\":1,\"e\":1},{\"id\":2,\"bad\":3}]",
    };

    for (size_t i = 0U; i < ARRAY_SIZE(invalid); i++) {
        struct codex_lighting_model after;

        zassert_equal(thstatus(invalid[i]), -EINVAL, "%s", invalid[i]);
        after = codex_lighting_snapshot();
        zassert_mem_equal(&after, &before, sizeof(before), "%s", invalid[i]);
    }
    zassert_equal(changed_count, 3U);
}

ZTEST(lighting, test_keys_ambient_off_preserves_agents_then_agents_off_completes_all_off)
{
    zassert_ok(rgbcfg("{\"keys\":{\"e\":1},\"ambient\":{\"e\":1}}"));
    zassert_ok(thstatus("[{\"id\":0,\"e\":1},{\"id\":1,\"e\":1},"
                        "{\"id\":2,\"e\":1},{\"id\":3,\"e\":1},"
                        "{\"id\":4,\"e\":1},{\"id\":5,\"e\":1}]"));
    zassert_ok(rgbcfg("{\"keys\":{\"e\":0},\"ambient\":{\"e\":0}}"));
    struct codex_lighting_model model = codex_lighting_snapshot();

    zassert_false(codex_lighting_zone_on(&model.keys));
    zassert_false(codex_lighting_zone_on(&model.ambient));
    zassert_true(codex_lighting_zone_on(&model.agents[0].zone));
    zassert_false(codex_lighting_all_off(&model));

    zassert_ok(thstatus("[{\"id\":0,\"e\":0},{\"id\":1,\"e\":0},"
                        "{\"id\":2,\"e\":0},{\"id\":3,\"e\":0},"
                        "{\"id\":4,\"e\":0},{\"id\":5,\"e\":0}]"));
    model = codex_lighting_snapshot();
    zassert_true(codex_lighting_all_off(&model));
}

ZTEST(lighting, test_no_effective_change_does_not_notify_renderer_seam)
{
    zassert_ok(rgbcfg("{\"keys\":{\"e\":0}}"));
    zassert_equal(changed_count, 0U);
    zassert_ok(thstatus("[{\"id\":3,\"e\":0,\"sk\":0}]"));
    zassert_equal(changed_count, 0U);
}

struct concurrency_ctx {
    struct k_sem start;
    struct k_sem done;
    atomic_t failures;
};

K_THREAD_STACK_DEFINE(reader_stack, 2048);
static struct k_thread reader_thread;
K_THREAD_STACK_DEFINE(keys_writer_stack, 2048);
K_THREAD_STACK_DEFINE(ambient_writer_stack, 2048);
static struct k_thread keys_writer_thread;
static struct k_thread ambient_writer_thread;

struct writer_ctx {
    const char *json;
    struct k_sem *start;
    struct k_sem *done;
    int result;
};

static void partial_writer(void *arg1, void *arg2, void *arg3)
{
    struct writer_ctx *ctx = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    k_sem_take(ctx->start, K_FOREVER);
    ctx->result = rgbcfg(ctx->json);
    k_sem_give(ctx->done);
}

static void snapshot_reader(void *arg1, void *arg2, void *arg3)
{
    struct concurrency_ctx *ctx = arg1;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    k_sem_take(&ctx->start, K_FOREVER);
    for (size_t i = 0U; i < 2000U; i++) {
        struct codex_lighting_model model = codex_lighting_snapshot();

        if (model.keys.color != model.ambient.color ||
            model.keys.effect != model.ambient.effect) {
            atomic_inc(&ctx->failures);
        }
    }
    k_sem_give(&ctx->done);
}

ZTEST(lighting, test_concurrent_snapshots_never_observe_torn_multi_zone_commit)
{
    struct concurrency_ctx ctx;
    k_sem_init(&ctx.start, 0, 1);
    k_sem_init(&ctx.done, 0, 1);
    atomic_clear(&ctx.failures);
    k_thread_create(&reader_thread, reader_stack, K_THREAD_STACK_SIZEOF(reader_stack),
                    snapshot_reader, &ctx, NULL, NULL, K_PRIO_PREEMPT(0), 0,
                    K_NO_WAIT);
    k_sem_give(&ctx.start);
    for (size_t i = 0U; i < 500U; i++) {
        const char *value = (i & 1U) != 0U
                                ? "{\"keys\":{\"e\":1,\"c\":1},"
                                  "\"ambient\":{\"e\":1,\"c\":1}}"
                                : "{\"keys\":{\"e\":2,\"c\":2},"
                                  "\"ambient\":{\"e\":2,\"c\":2}}";

        zassert_ok(rgbcfg(value));
        k_yield();
    }
    zassert_ok(k_sem_take(&ctx.done, K_SECONDS(2)));
    zassert_equal(atomic_get(&ctx.failures), 0);
}

ZTEST(lighting, test_concurrent_partial_writers_merge_against_locked_current_model)
{
    struct k_sem start;
    struct k_sem done;
    struct writer_ctx keys = {
        .json = "{\"keys\":{\"e\":1,\"c\":1}}",
        .start = &start,
        .done = &done,
    };
    struct writer_ctx ambient = {
        .json = "{\"ambient\":{\"e\":2,\"c\":2}}",
        .start = &start,
        .done = &done,
    };

    k_sem_init(&start, 0, 2);
    k_sem_init(&done, 0, 2);
    k_sem_init(&commit_barrier_release, 0, 2);
    atomic_clear(&commit_barrier_arrivals);
    commit_barrier_enabled = true;
    k_thread_create(&keys_writer_thread, keys_writer_stack,
                    K_THREAD_STACK_SIZEOF(keys_writer_stack), partial_writer,
                    &keys, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
    k_thread_create(&ambient_writer_thread, ambient_writer_stack,
                    K_THREAD_STACK_SIZEOF(ambient_writer_stack), partial_writer,
                    &ambient, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
    k_sem_give(&start);
    k_sem_give(&start);
    zassert_ok(k_sem_take(&done, K_SECONDS(2)));
    zassert_ok(k_sem_take(&done, K_SECONDS(2)));
    commit_barrier_enabled = false;
    zassert_equal(atomic_get(&commit_barrier_arrivals), 2);
    zassert_ok(keys.result);
    zassert_ok(ambient.result);

    struct codex_lighting_model model = codex_lighting_snapshot();

    zassert_equal(model.keys.effect, CODEX_EFFECT_SOLID);
    zassert_equal(model.keys.color, 1U);
    zassert_equal(model.ambient.effect, CODEX_EFFECT_SNAKE);
    zassert_equal(model.ambient.color, 2U);
}

ZTEST_SUITE(lighting, NULL, NULL, before, NULL, NULL);
