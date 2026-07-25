#include <codex/input.h>
#include <codex/transport.h>

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "input_internal.h"

#define CODEX_INPUT_JSON_MAX 64U

struct codex_input_mapping {
    const char *id;
    enum codex_input_event event;
    uint8_t agent_index;
};

struct codex_input_item {
    enum codex_input_event event;
    bool pressed;
};

static const struct codex_input_mapping key_mappings[] = {
    {"AG00", CODEX_INPUT_EVENT_AG00, 0U},
    {"AG01", CODEX_INPUT_EVENT_AG01, 1U},
    {"AG02", CODEX_INPUT_EVENT_AG02, 2U},
    {"AG03", CODEX_INPUT_EVENT_AG03, 3U},
    {"AG04", CODEX_INPUT_EVENT_AG04, 4U},
    {"AG05", CODEX_INPUT_EVENT_AG05, 5U},
    {"ACT06", CODEX_INPUT_EVENT_ACT06, 0U},
    {"ACT07", CODEX_INPUT_EVENT_ACT07, 0U},
    {"ACT08", CODEX_INPUT_EVENT_ACT08, 0U},
    {"ACT09", CODEX_INPUT_EVENT_ACT09, 0U},
    {"ACT10", CODEX_INPUT_EVENT_ACT10, 0U},
    {"ACT11", CODEX_INPUT_EVENT_ACT11, 0U},
    {"ACT12", CODEX_INPUT_EVENT_ACT12, 0U},
};

static const struct codex_input_mapping event_mappings[] = {
    [CODEX_INPUT_EVENT_AG00] = {"AG00", CODEX_INPUT_EVENT_AG00, 0U},
    [CODEX_INPUT_EVENT_AG01] = {"AG01", CODEX_INPUT_EVENT_AG01, 1U},
    [CODEX_INPUT_EVENT_AG02] = {"AG02", CODEX_INPUT_EVENT_AG02, 2U},
    [CODEX_INPUT_EVENT_AG03] = {"AG03", CODEX_INPUT_EVENT_AG03, 3U},
    [CODEX_INPUT_EVENT_AG04] = {"AG04", CODEX_INPUT_EVENT_AG04, 4U},
    [CODEX_INPUT_EVENT_AG05] = {"AG05", CODEX_INPUT_EVENT_AG05, 5U},
    [CODEX_INPUT_EVENT_ACT06] = {"ACT06", CODEX_INPUT_EVENT_ACT06, 0U},
    [CODEX_INPUT_EVENT_ACT07] = {"ACT07", CODEX_INPUT_EVENT_ACT07, 0U},
    [CODEX_INPUT_EVENT_ACT08] = {"ACT08", CODEX_INPUT_EVENT_ACT08, 0U},
    [CODEX_INPUT_EVENT_ACT09] = {"ACT09", CODEX_INPUT_EVENT_ACT09, 0U},
    [CODEX_INPUT_EVENT_ACT10] = {"ACT10", CODEX_INPUT_EVENT_ACT10, 0U},
    [CODEX_INPUT_EVENT_ACT11] = {"ACT11", CODEX_INPUT_EVENT_ACT11, 0U},
    [CODEX_INPUT_EVENT_ACT12] = {"ACT12", CODEX_INPUT_EVENT_ACT12, 0U},
    [CODEX_INPUT_EVENT_ENC_CLK] = {"ENC_CLK", CODEX_INPUT_EVENT_ENC_CLK, 0U},
    [CODEX_INPUT_EVENT_ENC_CW] = {"ENC_CW", CODEX_INPUT_EVENT_ENC_CW, 0U},
    [CODEX_INPUT_EVENT_ENC_CC] = {"ENC_CC", CODEX_INPUT_EVENT_ENC_CC, 0U},
};

K_MSGQ_DEFINE(input_event_queue, sizeof(struct codex_input_item),
              CONFIG_CODEX_INPUT_EVENT_QUEUE_DEPTH, 4);

static bool process_one(void)
{
    struct codex_input_item item;
    const struct codex_input_mapping *mapping;
    uint8_t json[CODEX_INPUT_JSON_MAX];
    int len;

    if (k_msgq_get(&input_event_queue, &item, K_NO_WAIT) != 0) {
        return false;
    }
    if (item.event >= CODEX_INPUT_EVENT_COUNT) {
        return true;
    }
    mapping = &event_mappings[item.event];
    len = snprintk((char *)json, sizeof(json),
                   "{\"m\":\"v.oai.hid\",\"p\":{\"k\":\"%s\",\"act\":%u,\"ag\":%u}}",
                   mapping->id, item.pressed ? 1U : 0U, mapping->agent_index);
    if (len < 0 || len >= (int)sizeof(json)) {
        return true;
    }
    /* Dispatch errors consume this owned event; only dequeue controls draining. */
    (void)codex_router_send_json(CODEX_CHANNEL_RPC, json, (size_t)len);
    return true;
}

static void input_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    while (process_one()) {
    }
}

K_WORK_DEFINE(input_work, input_work_handler);

int codex_input_enqueue_event(enum codex_input_event event, bool pressed)
{
    const struct codex_input_item item = {
        .event = event,
        .pressed = pressed,
    };

    if (event >= CODEX_INPUT_EVENT_COUNT) {
        return -EINVAL;
    }
    if (k_msgq_put(&input_event_queue, &item, K_NO_WAIT) != 0) {
        return -ENOSPC;
    }
    (void)k_work_submit(&input_work);
    return 0;
}

int codex_input_key(const char *id, bool pressed, uint8_t agent_index)
{
    if (id == NULL) {
        return -EINVAL;
    }
    for (size_t index = 0U; index < ARRAY_SIZE(key_mappings); index++) {
        const struct codex_input_mapping *mapping = &key_mappings[index];

        if (strcmp(id, mapping->id) == 0 && agent_index == mapping->agent_index) {
            return codex_input_enqueue_event(mapping->event, pressed);
        }
    }
    return -EINVAL;
}

#if defined(CONFIG_ZTEST)
void codex_input_test_reset(void)
{
    k_msgq_purge(&input_event_queue);
}
#endif
