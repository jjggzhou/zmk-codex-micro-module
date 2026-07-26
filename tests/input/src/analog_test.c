#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/input/input.h>
#include <zephyr/ztest.h>

#include <codex/input.h>

#include "input_test_support.h"

/* These test-only hooks feed the same input callback that production registers. */
static void configure_default(void)
{
    const struct codex_analog_calibration calibration = {
        .center_x = DT_PROP(DT_NODELABEL(codex_analog_test), center_x),
        .center_y = DT_PROP(DT_NODELABEL(codex_analog_test), center_y),
        .max_x = DT_PROP(DT_NODELABEL(codex_analog_test), max_x),
        .max_y = DT_PROP(DT_NODELABEL(codex_analog_test), max_y),
        .dead_zone = DT_PROP(DT_NODELABEL(codex_analog_test), dead_zone),
        .invert_x = false,
        .invert_y = false,
        .meaningful_delta = DT_PROP(DT_NODELABEL(codex_analog_test), meaningful_delta),
        .refresh_interval_ms = 20,
    };

    zassert_ok(codex_analog_test_configure(&calibration));
}

static void reset_analog(void *fixture)
{
    ARG_UNUSED(fixture);
    codex_input_test_reset_capture();
    codex_analog_test_reset();
    configure_default();
    codex_analog_test_set_uptime(0U);
}

static void cleanup_analog(void *fixture)
{
    ARG_UNUSED(fixture);
    codex_analog_test_reset();
}

ZTEST(analog, test_center_is_exact_zero)
{
    struct codex_radial value = codex_analog_normalize(2048, 2048);

    zassert_equal(value.distance, 0.0f);
    zassert_equal(value.angle, 0.0f);
}

ZTEST(analog, test_cardinal_and_wrapped_quadrant_angles_are_normalized)
{
    struct codex_radial right = codex_analog_normalize(4095, 2048);
    struct codex_radial up = codex_analog_normalize(2048, 4095);
    struct codex_radial left = codex_analog_normalize(0, 2048);
    struct codex_radial down = codex_analog_normalize(2048, 0);
    struct codex_radial lower_right = codex_analog_normalize(4095, 0);

    zassert_within(right.angle, 0.0f, 0.01f);
    zassert_within(up.angle, 0.25f, 0.01f);
    zassert_within(left.angle, 0.5f, 0.01f);
    zassert_within(down.angle, 0.75f, 0.01f);
    zassert_true(lower_right.angle > 0.75f && lower_right.angle < 1.0f);
}

ZTEST(analog, test_dead_zone_edge_and_circular_clamp)
{
    struct codex_radial inside = codex_analog_normalize(2148, 2048);
    struct codex_radial outside = codex_analog_normalize(2149, 2048);
    struct codex_radial diagonal = codex_analog_normalize(4095, 4095);

    zassert_equal(inside.distance, 0.0f);
    zassert_true(outside.distance > 0.0f);
    zassert_within(diagonal.distance, 1.0f, 0.001f);
}

ZTEST(analog, test_non_symmetric_axis_calibration_and_inversion)
{
    const struct codex_analog_calibration calibration = {
        .center_x = 1000,
        .center_y = 3000,
        .max_x = 3000,
        .max_y = 3500,
        .dead_zone = 0,
        .invert_x = true,
        .invert_y = false,
        .meaningful_delta = 100,
        .refresh_interval_ms = 20,
    };
    struct codex_radial value;

    zassert_ok(codex_analog_test_configure(&calibration));
    value = codex_analog_normalize(3000, 3000);
    zassert_within(value.angle, 0.5f, 0.01f);
    zassert_within(value.distance, 1.0f, 0.001f);
}

ZTEST(analog, test_input_pair_waits_for_sync_and_uses_last_other_axis)
{
    zassert_ok(codex_analog_test_input_event(INPUT_REL_X, 4095, false));
    k_sleep(K_MSEC(2));
    zassert_equal(codex_input_test_event_count(), 0U);
    zassert_ok(codex_analog_test_input_event(INPUT_REL_Y, 2048, true));
    codex_input_test_wait_for_events(1U);
    zassert_equal(strcmp(codex_input_test_event(0U),
                         "{\"m\":\"v.oai.rad\",\"p\":{\"a\":0,\"d\":1}}"), 0);

    zassert_ok(codex_analog_test_input_event(INPUT_REL_X, 2048, true));
    codex_input_test_wait_for_events(2U);
    zassert_equal(strcmp(codex_input_test_event(1U),
                         "{\"m\":\"v.oai.rad\",\"p\":{\"a\":0,\"d\":0}}"), 0);
}

ZTEST(analog, test_registered_callback_uses_center_for_first_pure_axis_and_filters_events)
{
    const struct device *source = DEVICE_DT_GET(DT_NODELABEL(analog_input_test));

    zassert_ok(input_report(NULL, INPUT_EV_REL, INPUT_REL_X, 4095, true, K_NO_WAIT));
    k_sleep(K_MSEC(2));
    zassert_equal(codex_input_test_event_count(), 0U);
    zassert_ok(input_report(source, INPUT_EV_KEY, INPUT_REL_X, 4095, true, K_NO_WAIT));
    zassert_ok(input_report(source, INPUT_EV_REL, INPUT_REL_WHEEL, 4095, true, K_NO_WAIT));
    k_sleep(K_MSEC(2));
    zassert_equal(codex_input_test_event_count(), 0U);

    zassert_ok(input_report(source, INPUT_EV_REL, INPUT_REL_X, 4095, true, K_NO_WAIT));
    codex_input_test_wait_for_events(1U);
    zassert_equal(strcmp(codex_input_test_event(0U),
                         "{\"m\":\"v.oai.rad\",\"p\":{\"a\":0,\"d\":1}}"), 0);
}

ZTEST(analog, test_registered_callback_first_pure_y_and_sync_boundary_use_cached_center)
{
    const struct device *source = DEVICE_DT_GET(DT_NODELABEL(analog_input_test));

    zassert_ok(input_report(source, INPUT_EV_REL, INPUT_REL_Y, 4095, false, K_NO_WAIT));
    k_sleep(K_MSEC(2));
    zassert_equal(codex_input_test_event_count(), 0U);
    zassert_ok(input_report(source, INPUT_EV_REL, INPUT_REL_Y, 4095, true, K_NO_WAIT));
    codex_input_test_wait_for_events(1U);
    zassert_equal(strcmp(codex_input_test_event(0U),
                         "{\"m\":\"v.oai.rad\",\"p\":{\"a\":0.25,\"d\":1}}"), 0);
}

ZTEST(analog, test_patched_upstream_driver_reports_changed_frame_then_returns_on_unchanged_frame)
{
    const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc0));
    const struct device *source = DEVICE_DT_GET(DT_NODELABEL(analog_input_test));

    zassert_true(device_is_ready(adc));
    zassert_true(device_is_ready(source));
    /* The pinned driver initializes its ADC sequence asynchronously. */
    k_sleep(K_MSEC(30));
    zassert_ok(adc_emul_const_value_set(adc, 0U, 4095U));
    zassert_ok(adc_emul_const_value_set(adc, 1U, 2048U));
    zassert_ok(sensor_sample_fetch(source));
    codex_input_test_wait_for_events(1U);

    /* This invokes the patched reverse scan with every channel unchanged. */
    zassert_ok(sensor_sample_fetch(source));
    k_sleep(K_MSEC(2));
    zassert_equal(codex_input_test_event_count(), 1U);
}

ZTEST(analog, test_held_position_refreshes_without_new_input_and_center_is_not_suppressed)
{
    zassert_ok(codex_analog_test_input_event(INPUT_REL_X, 4095, false));
    zassert_ok(codex_analog_test_input_event(INPUT_REL_Y, 2048, true));
    codex_input_test_wait_for_events(1U);

    /* No subsequent input arrives: the delayable work must refresh the hold. */
    codex_input_test_wait_for_events(2U);
    zassert_ok(codex_analog_test_input_event(INPUT_REL_X, 4080, true));

    codex_analog_test_set_uptime(20U);
    zassert_ok(codex_analog_test_input_event(INPUT_REL_X, 4080, true));
    codex_input_test_wait_for_events(3U);
    zassert_ok(codex_analog_test_input_event(INPUT_REL_X, 2048, true));
    codex_input_test_wait_for_events(4U);
    zassert_equal(strcmp(codex_input_test_event(3U),
                         "{\"m\":\"v.oai.rad\",\"p\":{\"a\":0,\"d\":0}}"), 0);
}

ZTEST(analog, test_center_is_emitted_once_and_can_be_emitted_again)
{
    zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.0f, .distance = 0.0f}));
    codex_input_test_wait_for_events(1U);
    zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.0f, .distance = 0.0f}));
    k_sleep(K_MSEC(2));
    zassert_equal(codex_input_test_event_count(), 1U);
    zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.25f, .distance = 0.8f}));
    codex_input_test_wait_for_events(2U);
    zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.0f, .distance = 0.0f}));
    codex_input_test_wait_for_events(3U);
}

ZTEST(analog, test_invalid_radial_values_are_rejected)
{
    zassert_equal(codex_input_radial_emit((struct codex_radial){.angle = NAN, .distance = 0.5f}),
                  -EINVAL);
    zassert_equal(codex_input_radial_emit((struct codex_radial){.angle = 0.0f, .distance = NAN}),
                  -EINVAL);
    zassert_equal(codex_input_radial_emit((struct codex_radial){.angle = 0.0f, .distance = 1.1f}),
                  -EINVAL);
}

ZTEST(analog, test_queue_full_and_router_error_do_not_stop_worker)
{
    const struct codex_analog_calibration calibration = {
        .center_x = 2048,
        .center_y = 2048,
        .max_x = 4095,
        .max_y = 4095,
        .dead_zone = 100,
        .meaningful_delta = 0,
        .refresh_interval_ms = 20,
    };

    zassert_ok(codex_analog_test_configure(&calibration));
    codex_input_test_set_block_router(true);
    zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.1f, .distance = 0.5f}));
    zassert_ok(codex_input_test_wait_router_entered());
    for (size_t i = 0U; i < CONFIG_CODEX_ANALOG_QUEUE_DEPTH; i++) {
        zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.1f + (float)i / 100.0f,
                                                                    .distance = 0.5f}));
    }
    zassert_equal(codex_input_radial_emit((struct codex_radial){.angle = 0.9f, .distance = 0.5f}),
                  -ENOSPC);
    codex_input_test_release_router();
    codex_input_test_set_block_router(false);
    codex_input_test_wait_for_events(CONFIG_CODEX_ANALOG_QUEUE_DEPTH + 1U);

    codex_input_test_set_router_result(-ENOMSG);
    zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.3f, .distance = 0.5f}));
    codex_input_test_wait_for_events(CONFIG_CODEX_ANALOG_QUEUE_DEPTH + 2U);
    codex_input_test_set_router_result(0);
    zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.4f, .distance = 0.5f}));
    codex_input_test_wait_for_events(CONFIG_CODEX_ANALOG_QUEUE_DEPTH + 3U);
}

ZTEST(analog, test_blocked_worker_coalesces_final_center_and_cancels_stale_refresh)
{
    zassert_ok(codex_analog_test_input_event(INPUT_REL_X, 4095, false));
    zassert_ok(codex_analog_test_input_event(INPUT_REL_Y, 2048, true));
    codex_input_test_wait_for_events(1U);

    codex_input_test_set_block_router(true);
    zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.1f, .distance = 0.8f}));
    zassert_ok(codex_input_test_wait_router_entered());
    for (size_t i = 0U; i < CONFIG_CODEX_ANALOG_QUEUE_DEPTH + 2U; i++) {
        zassert_ok(codex_analog_test_input_event(INPUT_REL_X, 4095 - (int32_t)i, true));
    }
    zassert_ok(codex_analog_test_input_event(INPUT_REL_X, 2048, true));
    codex_input_test_release_router();
    codex_input_test_set_block_router(false);
    codex_input_test_wait_for_events(3U);
    zassert_equal(strcmp(codex_input_test_event(2U),
                         "{\"m\":\"v.oai.rad\",\"p\":{\"a\":0,\"d\":0}}"), 0);
    k_sleep(K_MSEC(40));
    zassert_equal(codex_input_test_event_count(), 3U);
}

ZTEST(analog, test_blocked_worker_coalesces_final_noncenter_and_router_error_continues)
{
    codex_input_test_set_block_router(true);
    zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.1f, .distance = 0.4f}));
    zassert_ok(codex_input_test_wait_router_entered());
    for (size_t i = 0U; i < CONFIG_CODEX_ANALOG_QUEUE_DEPTH + 2U; i++) {
        zassert_ok(codex_analog_test_input_event(INPUT_REL_X, 2048 + (int32_t)i, true));
    }
    zassert_ok(codex_analog_test_input_event(INPUT_REL_X, 4095, true));
    codex_input_test_release_router();
    codex_input_test_set_block_router(false);
    codex_input_test_wait_for_events(2U);
    zassert_true(strstr(codex_input_test_event(1U), "\"d\":1") != NULL);

    codex_input_test_set_router_result(-ENOMSG);
    zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.3f, .distance = 0.6f}));
    codex_input_test_wait_for_events(3U);
    codex_input_test_set_router_result(0);
    zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.4f, .distance = 0.6f}));
    codex_input_test_wait_for_events(4U);
}

ZTEST(analog, test_refresh_comparison_is_wrap_safe)
{
    codex_analog_test_set_uptime(UINT32_MAX - 5U);
    zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.2f, .distance = 0.5f}));
    codex_input_test_wait_for_events(1U);
    codex_analog_test_set_uptime(15U);
    zassert_ok(codex_input_radial_emit((struct codex_radial){.angle = 0.2f, .distance = 0.5f}));
    codex_input_test_wait_for_events(2U);
}

ZTEST_SUITE(analog, NULL, NULL, reset_analog, cleanup_analog, NULL);
