#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <codex/framing.h>
#include <codex/input.h>

#define CAPTURE_MAX 48U

static char sent[CAPTURE_MAX][80];
static size_t sent_count;
static int router_result;
static int router_scripted_results[CAPTURE_MAX];
static size_t router_scripted_count;
static bool block_router;
K_SEM_DEFINE(router_entered, 0, 1);
K_SEM_DEFINE(router_release, 0, 1);
K_SEM_DEFINE(blocker_entered, 0, 1);
K_SEM_DEFINE(blocker_release, 0, 1);

static void system_work_blocker(struct k_work *work)
{
    ARG_UNUSED(work);
    k_sem_give(&blocker_entered);
    (void)k_sem_take(&blocker_release, K_FOREVER);
}

K_WORK_DEFINE(blocker_work, system_work_blocker);

int codex_router_send_json(enum codex_channel channel, const uint8_t *json,
                           size_t len)
{
    zassert_equal(channel, CODEX_CHANNEL_RPC);
    zassert_true(sent_count < CAPTURE_MAX);
    zassert_true(len < sizeof(sent[0]));
    memcpy(sent[sent_count], json, len);
    sent[sent_count][len] = '\0';
    sent_count++;
    if (block_router) {
        k_sem_give(&router_entered);
        (void)k_sem_take(&router_release, K_FOREVER);
    }
    if (sent_count <= router_scripted_count) {
        return router_scripted_results[sent_count - 1U];
    }
    return router_result;
}

static void reset_case(void *fixture)
{
    ARG_UNUSED(fixture);
    memset(sent, 0, sizeof(sent));
    sent_count = 0U;
    router_result = 0;
    memset(router_scripted_results, 0, sizeof(router_scripted_results));
    router_scripted_count = 0U;
    block_router = false;
    k_sem_reset(&router_entered);
    k_sem_reset(&router_release);
    k_sem_reset(&blocker_entered);
    k_sem_reset(&blocker_release);
    codex_input_test_reset();
}

static void wait_for_events(size_t count)
{
    int64_t deadline = k_uptime_get() + 200;

    while (sent_count < count && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_equal(sent_count, count);
}

static void assert_event(size_t index, const char *key, unsigned int action,
                         unsigned int agent)
{
    char expected[80];

    snprintk(expected, sizeof(expected),
             "{\"m\":\"v.oai.hid\",\"p\":{\"k\":\"%s\",\"act\":%u,\"ag\":%u}}",
             key, action, agent);
    zassert_equal(strcmp(sent[index], expected), 0, "%s", sent[index]);
}

ZTEST(input, test_all_agent_keys_use_their_matching_agent_index)
{
    static const char *const ids[] = {"AG00", "AG01", "AG02", "AG03", "AG04", "AG05"};

    for (uint8_t agent = 0U; agent < ARRAY_SIZE(ids); agent++) {
        zassert_ok(codex_input_key(ids[agent], true, agent));
        zassert_ok(codex_input_key(ids[agent], false, agent));
    }
    wait_for_events(ARRAY_SIZE(ids) * 2U);
    for (uint8_t agent = 0U; agent < ARRAY_SIZE(ids); agent++) {
        assert_event(agent * 2U, ids[agent], 1U, agent);
        assert_event(agent * 2U + 1U, ids[agent], 0U, agent);
    }
}

ZTEST(input, test_repeated_edges_are_forwarded_without_local_deduplication)
{
    zassert_ok(codex_input_key("ACT10", true, 0U));
    zassert_ok(codex_input_key("ACT10", true, 0U));
    zassert_ok(codex_input_key("ACT10", false, 0U));
    zassert_ok(codex_input_key("ACT10", false, 0U));
    wait_for_events(4U);

    assert_event(0U, "ACT10", 1U, 0U);
    assert_event(1U, "ACT10", 1U, 0U);
    assert_event(2U, "ACT10", 0U, 0U);
    assert_event(3U, "ACT10", 0U, 0U);
}

ZTEST(input, test_all_action_keys_emit_each_uncoalesced_edge)
{
    static const char *const ids[] = {"ACT06", "ACT07", "ACT08", "ACT09", "ACT10", "ACT11", "ACT12"};

    for (size_t index = 0U; index < ARRAY_SIZE(ids); index++) {
        zassert_ok(codex_input_key(ids[index], true, 0U));
        zassert_ok(codex_input_key(ids[index], false, 0U));
    }
    wait_for_events(ARRAY_SIZE(ids) * 2U);
    for (size_t index = 0U; index < ARRAY_SIZE(ids); index++) {
        assert_event(index * 2U, ids[index], 1U, 0U);
        assert_event(index * 2U + 1U, ids[index], 0U, 0U);
    }
}

ZTEST(input, test_act10_and_act11_solo_overlap_and_interleaved_edges_remain_independent)
{
    zassert_ok(codex_input_key("ACT10", true, 0U));
    zassert_ok(codex_input_key("ACT11", true, 0U));
    zassert_ok(codex_input_key("ACT10", false, 0U));
    zassert_ok(codex_input_key("ACT11", false, 0U));
    zassert_ok(codex_input_key("ACT11", true, 0U));
    zassert_ok(codex_input_key("ACT11", false, 0U));
    zassert_ok(codex_input_key("ACT10", true, 0U));
    zassert_ok(codex_input_key("ACT11", true, 0U));
    zassert_ok(codex_input_key("ACT11", false, 0U));
    zassert_ok(codex_input_key("ACT10", false, 0U));
    wait_for_events(10U);

    assert_event(0U, "ACT10", 1U, 0U);
    assert_event(1U, "ACT11", 1U, 0U);
    assert_event(2U, "ACT10", 0U, 0U);
    assert_event(3U, "ACT11", 0U, 0U);
    assert_event(4U, "ACT11", 1U, 0U);
    assert_event(5U, "ACT11", 0U, 0U);
    assert_event(6U, "ACT10", 1U, 0U);
    assert_event(7U, "ACT11", 1U, 0U);
    assert_event(8U, "ACT11", 0U, 0U);
    assert_event(9U, "ACT10", 0U, 0U);
}

ZTEST(input, test_encoder_push_and_detents_use_exact_hardware_compatible_events)
{
    zassert_ok(codex_input_encoder_press(true));
    zassert_ok(codex_input_encoder_press(false));
    zassert_ok(codex_input_encoder_tick(1));
    zassert_ok(codex_input_encoder_tick(-1));
    wait_for_events(4U);

    assert_event(0U, "ENC_CLK", 1U, 0U);
    assert_event(1U, "ENC_CLK", 0U, 0U);
    /* Hardware-compatible detents are one momentary act:1 notification. */
    assert_event(2U, "ENC_CW", 1U, 0U);
    assert_event(3U, "ENC_CC", 1U, 0U);
}

ZTEST(input, test_rejects_unknown_or_mismatched_ids_without_emitting_json)
{
    zassert_equal(codex_input_key(NULL, true, 0U), -EINVAL);
    zassert_equal(codex_input_key("ACT10\",\"owned\":1", true, 0U), -EINVAL);
    zassert_equal(codex_input_key("AG03", true, 2U), -EINVAL);
    zassert_equal(codex_input_key("ACT10", true, 1U), -EINVAL);
    zassert_equal(codex_input_encoder_tick(0), -EINVAL);
    zassert_equal(codex_input_encoder_tick(2), -EINVAL);
    k_sleep(K_MSEC(5));
    zassert_equal(sent_count, 0U);
}

ZTEST(input, test_full_queue_returns_enospc_while_worker_is_blocked)
{
    block_router = true;
    zassert_ok(codex_input_key("ACT06", true, 0U));
    zassert_ok(k_sem_take(&router_entered, K_MSEC(100)));
    for (size_t index = 0U; index < CONFIG_CODEX_INPUT_EVENT_QUEUE_DEPTH; index++) {
        zassert_ok(codex_input_key("ACT07", true, 0U));
    }
    zassert_equal(codex_input_key("ACT08", true, 0U), -ENOSPC);
    k_sem_give(&router_release);
    block_router = false;
    wait_for_events(CONFIG_CODEX_INPUT_EVENT_QUEUE_DEPTH + 1U);
}

ZTEST(input, test_router_error_does_not_stop_the_production_worker)
{
    router_result = -ENOTCONN;
    zassert_ok(codex_input_key("ACT10", true, 0U));
    wait_for_events(1U);
    router_result = 0;
    zassert_ok(codex_input_key("ACT11", true, 0U));
    wait_for_events(2U);
    assert_event(0U, "ACT10", 1U, 0U);
    assert_event(1U, "ACT11", 1U, 0U);
}

ZTEST(input, test_router_enomsg_consumes_only_its_event_and_drains_fifo)
{
    zassert_true(k_work_submit(&blocker_work) >= 0);
    zassert_ok(k_sem_take(&blocker_entered, K_MSEC(100)));
    router_scripted_results[0] = -ENOMSG;
    router_scripted_count = 1U;
    zassert_ok(codex_input_key("ACT10", true, 0U));
    zassert_ok(codex_input_key("ACT11", true, 0U));
    k_sem_give(&blocker_release);
    wait_for_events(2U);
    assert_event(0U, "ACT10", 1U, 0U);
    assert_event(1U, "ACT11", 1U, 0U);
}

ZTEST_SUITE(input, NULL, NULL, reset_case, NULL, NULL);
