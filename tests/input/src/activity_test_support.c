#include <stddef.h>

#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include "activity_test_support.h"

LOG_MODULE_REGISTER(zmk, CONFIG_ZMK_LOG_LEVEL);

const struct zmk_event_type zmk_event_zmk_activity_state_changed = {
    .name = "zmk_activity_state_changed",
};
const struct zmk_event_type zmk_event_zmk_position_state_changed = {
    .name = "zmk_position_state_changed",
};
const struct zmk_event_type zmk_event_zmk_sensor_event = {
    .name = "zmk_sensor_event",
};

static size_t activity_state_event_count;

int raise_zmk_activity_state_changed(struct zmk_activity_state_changed event)
{
    ARG_UNUSED(event);
    activity_state_event_count++;
    return 0;
}

void codex_activity_test_reset_events(void)
{
    activity_state_event_count = 0U;
}

size_t codex_activity_test_event_count(void)
{
    return activity_state_event_count;
}
