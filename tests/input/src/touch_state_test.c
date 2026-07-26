#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <zmk/endpoints.h>
#include <zmk/keymap.h>

#include <codex/state.h>

extern struct k_work_delayable deadline_work;

static bool fake_layers[CODEX_LAYER_COUNT];
static uint8_t fake_layer_ids[CODEX_LAYER_COUNT];
static int fake_layer_activate_error;
static int fake_layer_deactivate_error;
static unsigned int fake_activate_no_mutation_calls;
static unsigned int fake_deactivate_no_mutation_calls;
static uint8_t fake_active_profile;
static int fake_profile_select_error;
static uint8_t fake_profile_on_error;
static enum zmk_transport fake_preferred_transport;
static struct zmk_endpoint_instance fake_selected_endpoint;
static int fake_endpoint_select_error;
static bool fake_usb_ready;
static bool fake_ble_ready;
static unsigned int fake_profile_select_calls;
static unsigned int fake_endpoint_select_calls;
static unsigned int fake_clear_calls;
static uint8_t fake_last_profile;
static enum zmk_transport fake_last_transport;
static uint8_t sink_bits;
static unsigned int sink_calls;

K_SEM_DEFINE(touch_blocker_entered, 0, 1);
K_SEM_DEFINE(touch_blocker_release, 0, 1);

static void touch_blocker_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    k_sem_give(&touch_blocker_entered);
    (void)k_sem_take(&touch_blocker_release, K_FOREVER);
}

K_WORK_DEFINE(touch_blocker, touch_blocker_handler);

uint8_t __wrap_zmk_keymap_highest_layer_active(void)
{
    for (int index = CODEX_LAYER_COUNT - 1; index >= 0; index--) {
        if (fake_layers[fake_layer_ids[index]]) {
            return (uint8_t)index;
        }
    }
    return 0U;
}

uint8_t __wrap_zmk_keymap_layer_index_to_id(uint8_t index)
{
    return index < CODEX_LAYER_COUNT ? fake_layer_ids[index] : UINT8_MAX;
}

bool __wrap_zmk_keymap_layer_active(uint8_t layer)
{
    return layer < CODEX_LAYER_COUNT && fake_layers[layer];
}

int __wrap_zmk_keymap_layer_activate(uint8_t layer)
{
    if (layer >= CODEX_LAYER_COUNT) {
        return -EINVAL;
    }
    if (fake_activate_no_mutation_calls > 0U) {
        fake_activate_no_mutation_calls--;
        return -EIO;
    }
    fake_layers[layer] = true;
    return fake_layer_activate_error;
}

int __wrap_zmk_keymap_layer_deactivate(uint8_t layer)
{
    if (layer >= CODEX_LAYER_COUNT) {
        return -EINVAL;
    }
    if (fake_deactivate_no_mutation_calls > 0U) {
        fake_deactivate_no_mutation_calls--;
        return -EIO;
    }
    if (layer != 0U) {
        fake_layers[layer] = false;
    }
    return fake_layer_deactivate_error;
}

int __wrap_zmk_ble_active_profile_index(void) { return fake_active_profile; }

int __wrap_zmk_ble_prof_select(uint8_t profile)
{
    fake_profile_select_calls++;
    fake_last_profile = profile;
    if (fake_profile_select_error != 0) {
        if (fake_profile_on_error != UINT8_MAX) {
            fake_active_profile = fake_profile_on_error;
        }
        return fake_profile_select_error;
    }
    fake_active_profile = profile;
    if (fake_selected_endpoint.transport == ZMK_TRANSPORT_BLE) {
        fake_selected_endpoint.ble.profile_index = profile;
    }
    return 0;
}

void __wrap_zmk_ble_clear_bonds(void) { fake_clear_calls++; }

int __wrap_zmk_endpoints_select_transport(enum zmk_transport transport)
{
    fake_endpoint_select_calls++;
    fake_last_transport = transport;
    if (fake_endpoint_select_error != 0) {
        return fake_endpoint_select_error;
    }
    fake_preferred_transport = transport;
    if (transport == ZMK_TRANSPORT_USB && fake_usb_ready) {
        fake_selected_endpoint.transport = ZMK_TRANSPORT_USB;
    } else if (transport == ZMK_TRANSPORT_BLE && fake_ble_ready) {
        fake_selected_endpoint.transport = ZMK_TRANSPORT_BLE;
        fake_selected_endpoint.ble.profile_index = fake_active_profile;
    } else if (fake_ble_ready) {
        fake_selected_endpoint.transport = ZMK_TRANSPORT_BLE;
        fake_selected_endpoint.ble.profile_index = fake_active_profile;
    } else if (fake_usb_ready) {
        fake_selected_endpoint.transport = ZMK_TRANSPORT_USB;
    }
    return 0;
}

struct zmk_endpoint_instance __wrap_zmk_endpoints_selected(void)
{
    return fake_selected_endpoint;
}

void codex_indicator_sink(uint8_t bits)
{
    sink_bits = bits;
    sink_calls++;
}

static void drain(void) { codex_touch_test_drain(); }

static void edge(bool touched, int64_t now_ms)
{
    codex_touch_edge(touched, now_ms);
    drain();
}

static void tick(int64_t now_ms)
{
    codex_connection_tick(now_ms);
    drain();
}

static void tap(int64_t pressed_ms, int64_t released_ms)
{
    edge(true, pressed_ms);
    edge(false, released_ms);
}

static void enter_mode(int64_t pressed_ms)
{
    edge(true, pressed_ms);
    tick(pressed_ms + CODEX_HOLD_MS);
    zassert_true(codex_connection_mode_active());
    edge(false, pressed_ms + CODEX_HOLD_MS);
}

static void reset_touch(void *fixture)
{
    ARG_UNUSED(fixture);
    memset(fake_layers, 0, sizeof(fake_layers));
    for (uint8_t index = 0U; index < CODEX_LAYER_COUNT; index++) {
        fake_layer_ids[index] = index;
    }
    fake_layers[0] = true;
    fake_layer_activate_error = 0;
    fake_layer_deactivate_error = 0;
    fake_activate_no_mutation_calls = 0U;
    fake_deactivate_no_mutation_calls = 0U;
    fake_active_profile = 0U;
    fake_profile_select_error = 0;
    fake_profile_on_error = UINT8_MAX;
    fake_preferred_transport = ZMK_TRANSPORT_BLE;
    fake_selected_endpoint = (struct zmk_endpoint_instance){
        .transport = ZMK_TRANSPORT_BLE,
        .ble = {.profile_index = 0U},
    };
    fake_endpoint_select_error = 0;
    fake_usb_ready = false;
    fake_ble_ready = true;
    fake_profile_select_calls = 0U;
    fake_endpoint_select_calls = 0U;
    fake_clear_calls = 0U;
    fake_last_profile = UINT8_MAX;
    fake_last_transport = ZMK_TRANSPORT_BLE;
    sink_bits = 0U;
    sink_calls = 0U;
    k_sem_reset(&touch_blocker_entered);
    k_sem_reset(&touch_blocker_release);
    codex_touch_test_reset();
}

ZTEST(touch_state, test_indicator_functions_define_top_middle_bottom_mapping)
{
    static const uint8_t layer_bits[] = {
        CODEX_INDICATOR_TOP,
        CODEX_INDICATOR_MIDDLE,
        CODEX_INDICATOR_BOTTOM,
        CODEX_INDICATOR_TOP | CODEX_INDICATOR_MIDDLE,
        CODEX_INDICATOR_MIDDLE | CODEX_INDICATOR_BOTTOM,
        CODEX_INDICATOR_TOP | CODEX_INDICATOR_MIDDLE | CODEX_INDICATOR_BOTTOM,
    };
    static const uint8_t connection_bits[] = {
        CODEX_INDICATOR_TOP,
        CODEX_INDICATOR_MIDDLE,
        CODEX_INDICATOR_BOTTOM,
        CODEX_INDICATOR_TOP | CODEX_INDICATOR_MIDDLE | CODEX_INDICATOR_BOTTOM,
    };

    for (uint8_t layer = 0U; layer < ARRAY_SIZE(layer_bits); layer++) {
        zassert_equal(codex_layer_indicator_bits(layer), layer_bits[layer]);
    }
    for (uint8_t choice = 0U; choice < ARRAY_SIZE(connection_bits); choice++) {
        zassert_equal(codex_connection_indicator_bits(choice), connection_bits[choice]);
    }
    zassert_equal(codex_layer_indicator_bits(CODEX_LAYER_COUNT), 0U);
    zassert_equal(codex_layer_indicator_bits(UINT8_MAX), 0U);
    zassert_equal(codex_connection_indicator_bits(CODEX_CONNECTION_CHOICE_COUNT), 0U);
    zassert_equal(codex_connection_indicator_bits(UINT8_MAX), 0U);
}

ZTEST(touch_state, test_six_short_taps_cycle_all_layers_and_real_indicator_state)
{
    static const uint8_t expected_bits[] = {2U, 1U, 6U, 3U, 7U, 4U};

    for (uint8_t expected_layer = 1U; expected_layer < CODEX_LAYER_COUNT;
         expected_layer++) {
        tap(expected_layer * 1000, expected_layer * 1000 + 100);
        zassert_equal(__wrap_zmk_keymap_highest_layer_active(), expected_layer);
        zassert_equal(codex_indicator_bits(), expected_bits[expected_layer - 1U]);
        zassert_equal(sink_bits, expected_bits[expected_layer - 1U]);
    }
    tap(6000, 6100);
    zassert_equal(__wrap_zmk_keymap_highest_layer_active(), 0U);
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_TOP);
}

ZTEST(touch_state, test_layer_indices_are_converted_to_stable_ids_when_reordered)
{
    static const uint8_t reordered_ids[CODEX_LAYER_COUNT] = {0U, 3U, 1U, 2U, 4U, 5U};

    memcpy(fake_layer_ids, reordered_ids, sizeof(fake_layer_ids));
    tap(0, 100);

    zassert_true(fake_layers[3U]);
    zassert_false(fake_layers[1U]);
    zassert_equal(__wrap_zmk_keymap_highest_layer_active(), 1U);
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_MIDDLE);
}

ZTEST(touch_state, test_activate_event_error_after_mutation_still_reconciles_exclusive_layer)
{
    tap(0, 100);
    fake_layer_activate_error = -EIO;
    tap(1000, 1100);

    zassert_false(fake_layers[1U]);
    zassert_true(fake_layers[2U]);
    zassert_equal(__wrap_zmk_keymap_highest_layer_active(), 2U);
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_BOTTOM);
    zassert_equal(codex_touch_diagnostics_get().layer_api_errors, 1U);
}

ZTEST(touch_state, test_deactivate_event_error_after_mutation_keeps_exclusive_target)
{
    tap(0, 100);
    fake_layer_deactivate_error = -EIO;
    tap(1000, 1100);

    zassert_false(fake_layers[1U]);
    zassert_true(fake_layers[2U]);
    zassert_equal(__wrap_zmk_keymap_highest_layer_active(), 2U);
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_BOTTOM);
}

ZTEST(touch_state, test_transient_no_mutation_error_is_reconciled_on_real_state)
{
    tap(0, 100);
    fake_deactivate_no_mutation_calls = 1U;
    tap(1000, 1100);

    zassert_false(fake_layers[1U]);
    zassert_true(fake_layers[2U]);
    zassert_equal(__wrap_zmk_keymap_highest_layer_active(), 2U);
}

ZTEST(touch_state, test_unreconciled_partial_change_rolls_back_original_exact_layers)
{
    tap(0, 100);
    fake_deactivate_no_mutation_calls = UINT8_MAX;
    tap(1000, 1100);

    zassert_true(fake_layers[1U]);
    zassert_false(fake_layers[2U]);
    zassert_equal(__wrap_zmk_keymap_highest_layer_active(), 1U);
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_MIDDLE);
}

ZTEST(touch_state, test_2999_is_tap_and_3000_is_one_hold_without_release_tap)
{
    edge(true, 0);
    tick(CODEX_HOLD_MS - 1);
    zassert_false(codex_connection_mode_active());
    edge(false, CODEX_HOLD_MS - 1);
    zassert_equal(__wrap_zmk_keymap_highest_layer_active(), 1U);

    edge(true, 10000);
    tick(10000 + CODEX_HOLD_MS);
    zassert_true(codex_connection_mode_active());
    zassert_equal(__wrap_zmk_keymap_highest_layer_active(), 1U);
    edge(false, 10000 + CODEX_HOLD_MS);
    zassert_equal(__wrap_zmk_keymap_highest_layer_active(), 1U);
}

ZTEST(touch_state, test_duplicate_edges_do_not_create_extra_taps_or_holds)
{
    edge(true, 0);
    edge(true, 1);
    edge(false, 100);
    edge(false, 101);
    zassert_equal(__wrap_zmk_keymap_highest_layer_active(), 1U);

    edge(true, 1000);
    edge(true, 1001);
    tick(4000);
    tick(5000);
    zassert_true(codex_connection_mode_active());
    edge(false, 5001);
    edge(false, 5002);
    zassert_equal(fake_clear_calls, 0U);
}

ZTEST(touch_state, test_mode_initial_choice_reflects_ble_profile_or_usb_endpoint)
{
    fake_active_profile = 2U;
    fake_selected_endpoint.ble.profile_index = 2U;
    enter_mode(0);
    zassert_equal(codex_connection_choice_get(), CODEX_CONNECTION_BLE_3);
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_BOTTOM);

    codex_touch_test_reset();
    fake_selected_endpoint.transport = ZMK_TRANSPORT_USB;
    enter_mode(10000);
    zassert_equal(codex_connection_choice_get(), CODEX_CONNECTION_USB);
    zassert_equal(codex_indicator_bits(), 7U);
}

ZTEST(touch_state, test_unsupported_active_ble_profiles_are_really_normalized_to_profile_zero)
{
    fake_active_profile = 3U;
    fake_selected_endpoint.ble.profile_index = 3U;
    enter_mode(0);
    zassert_equal(fake_profile_select_calls, 1U);
    zassert_equal(fake_last_profile, 0U);
    zassert_equal(fake_active_profile, 0U);
    zassert_equal(codex_connection_choice_get(), CODEX_CONNECTION_BLE_1);

    reset_touch(NULL);
    fake_active_profile = 4U;
    fake_selected_endpoint.ble.profile_index = 4U;
    enter_mode(10000);
    zassert_equal(fake_profile_select_calls, 1U);
    zassert_equal(fake_last_profile, 0U);
    zassert_equal(fake_active_profile, 0U);
    zassert_equal(codex_connection_choice_get(), CODEX_CONNECTION_BLE_1);
}

ZTEST(touch_state, test_unsupported_profile_select_failure_does_not_enter_mode)
{
    fake_active_profile = 4U;
    fake_selected_endpoint.ble.profile_index = 4U;
    fake_profile_select_error = -EIO;
    edge(true, 0);
    tick(CODEX_HOLD_MS);

    zassert_false(codex_connection_mode_active());
    zassert_equal(fake_profile_select_calls, 1U);
    zassert_equal(fake_active_profile, 4U);
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_TOP);
    zassert_equal(codex_touch_diagnostics_get().profile_api_errors, 1U);
    edge(false, CODEX_HOLD_MS);
    zassert_equal(__wrap_zmk_keymap_highest_layer_active(), 0U);
}

ZTEST(touch_state, test_profile_error_refresh_never_aliases_unsupported_profile_to_ble_one)
{
    enter_mode(0);
    fake_profile_select_error = -EIO;
    fake_profile_on_error = 4U;
    tap(4000, 4100);

    zassert_equal(fake_active_profile, 4U);
    zassert_false(codex_connection_mode_active());
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_TOP);
    zassert_equal(codex_touch_diagnostics_get().profile_api_errors, 1U);
}

ZTEST(touch_state, test_mode_taps_cycle_ble_profiles_and_usb_with_exact_calls)
{
    enter_mode(0);
    fake_profile_select_calls = 0U;
    fake_endpoint_select_calls = 0U;

    tap(4000, 4100);
    zassert_equal(codex_connection_choice_get(), CODEX_CONNECTION_BLE_2);
    zassert_equal(fake_last_transport, ZMK_TRANSPORT_BLE);
    zassert_equal(fake_last_profile, 1U);
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_MIDDLE);

    tap(5000, 5100);
    zassert_equal(codex_connection_choice_get(), CODEX_CONNECTION_BLE_3);
    zassert_equal(fake_last_profile, 2U);
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_BOTTOM);

    tap(6000, 6100);
    zassert_equal(codex_connection_choice_get(), CODEX_CONNECTION_USB);
    zassert_equal(fake_last_transport, ZMK_TRANSPORT_USB);
    zassert_equal(codex_indicator_bits(), 7U);

    tap(7000, 7100);
    zassert_equal(codex_connection_choice_get(), CODEX_CONNECTION_BLE_1);
    zassert_equal(fake_last_transport, ZMK_TRANSPORT_BLE);
    zassert_equal(fake_last_profile, 0U);
    zassert_equal(fake_profile_select_calls, 3U);
    zassert_equal(fake_endpoint_select_calls, 4U);
}

ZTEST(touch_state, test_usb_preference_does_not_claim_usb_ready_or_break_ble_fallback)
{
    enter_mode(0);
    tap(4000, 4100);
    tap(5000, 5100);
    tap(6000, 6100);

    zassert_equal(codex_connection_choice_get(), CODEX_CONNECTION_USB);
    zassert_equal(fake_preferred_transport, ZMK_TRANSPORT_USB);
    zassert_equal(fake_selected_endpoint.transport, ZMK_TRANSPORT_BLE);
    zassert_true(fake_ble_ready);
    zassert_false(fake_usb_ready);

    tick(6100 + CODEX_CONNECTION_IDLE_MS);
    zassert_false(codex_connection_mode_active());
    enter_mode(12000);
    zassert_equal(codex_connection_choice_get(), CODEX_CONNECTION_USB);
    zassert_equal(codex_indicator_bits(), 7U);
}

ZTEST(touch_state, test_ble_hold_selects_clears_and_reopens_once_but_usb_hold_is_safe)
{
    fake_active_profile = 1U;
    fake_selected_endpoint.ble.profile_index = 1U;
    enter_mode(0);
    fake_clear_calls = 0U;
    fake_profile_select_calls = 0U;
    fake_endpoint_select_calls = 0U;

    edge(true, 4000);
    tick(7000);
    tick(7001);
    edge(false, 7100);
    zassert_equal(fake_last_transport, ZMK_TRANSPORT_BLE);
    zassert_equal(fake_last_profile, 1U);
    zassert_equal(fake_profile_select_calls, 1U);
    zassert_equal(fake_endpoint_select_calls, 1U);
    zassert_equal(fake_clear_calls, 1U);

    tap(8000, 8100);
    tap(9000, 9100);
    zassert_equal(codex_connection_choice_get(), CODEX_CONNECTION_USB);
    edge(true, 10000);
    tick(13000);
    tick(13001);
    edge(false, 13100);
    zassert_equal(fake_clear_calls, 1U);
}

ZTEST(touch_state, test_connection_timeout_is_exact_and_interaction_renews_it)
{
    enter_mode(0);
    tick(CODEX_HOLD_MS + CODEX_CONNECTION_IDLE_MS - 1);
    zassert_true(codex_connection_mode_active());
    tick(CODEX_HOLD_MS + CODEX_CONNECTION_IDLE_MS);
    zassert_false(codex_connection_mode_active());
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_TOP);

    enter_mode(10000);
    tap(17000, 17100);
    tick(17100 + CODEX_CONNECTION_IDLE_MS - 1);
    zassert_true(codex_connection_mode_active());
    tick(17100 + CODEX_CONNECTION_IDLE_MS);
    zassert_false(codex_connection_mode_active());
}

ZTEST(touch_state, test_real_delayable_handler_uses_fake_clock_without_sleep)
{
    codex_touch_test_set_uptime(0);
    edge(true, 0);
    codex_touch_test_set_uptime(CODEX_HOLD_MS);
    codex_touch_test_fire_deadline();
    zassert_true(codex_connection_mode_active());
    edge(false, CODEX_HOLD_MS);

    codex_touch_test_set_uptime(CODEX_HOLD_MS + CODEX_CONNECTION_IDLE_MS);
    codex_touch_test_fire_deadline();
    zassert_false(codex_connection_mode_active());
}

ZTEST(touch_state, test_hold_deadline_remaining_uses_current_clock_after_worker_backlog)
{
    codex_touch_test_set_uptime(0);
    zassert_true(k_work_submit(&touch_blocker) >= 0);
    zassert_ok(k_sem_take(&touch_blocker_entered, K_MSEC(100)));
    codex_touch_edge(true, 0);
    codex_touch_test_set_uptime(2500);
    k_sem_give(&touch_blocker_release);
    drain();

    uint32_t remaining =
        k_ticks_to_ms_ceil32(k_work_delayable_remaining_get(&deadline_work));
    zassert_between_inclusive(remaining, 499U,
                              500U + k_ticks_to_ms_ceil32(1), "remaining=%u", remaining);

    codex_touch_test_set_uptime(CODEX_HOLD_MS);
    codex_touch_test_fire_deadline();
    zassert_true(codex_connection_mode_active());
}

ZTEST(touch_state, test_idle_deadline_remaining_uses_current_clock_after_worker_backlog)
{
    enter_mode(0);
    codex_touch_test_set_uptime(4000);
    zassert_true(k_work_submit(&touch_blocker) >= 0);
    zassert_ok(k_sem_take(&touch_blocker_entered, K_MSEC(100)));
    codex_touch_edge(true, 4000);
    codex_touch_edge(false, 4100);
    codex_touch_test_set_uptime(8600);
    k_sem_give(&touch_blocker_release);
    drain();

    uint32_t remaining =
        k_ticks_to_ms_ceil32(k_work_delayable_remaining_get(&deadline_work));
    zassert_between_inclusive(remaining, 499U,
                              500U + k_ticks_to_ms_ceil32(1), "remaining=%u", remaining);

    codex_touch_test_set_uptime(9100);
    codex_touch_test_fire_deadline();
    zassert_false(codex_connection_mode_active());
}

ZTEST(touch_state, test_edge_wins_same_deadline_race_when_queued_first)
{
    enter_mode(0);
    zassert_true(k_work_submit(&touch_blocker) >= 0);
    zassert_ok(k_sem_take(&touch_blocker_entered, K_MSEC(100)));
    codex_touch_edge(true, CODEX_HOLD_MS + CODEX_CONNECTION_IDLE_MS);
    codex_connection_tick(CODEX_HOLD_MS + CODEX_CONNECTION_IDLE_MS);
    k_sem_give(&touch_blocker_release);
    drain();
    zassert_true(codex_connection_mode_active());
    edge(false, CODEX_HOLD_MS + CODEX_CONNECTION_IDLE_MS + 1);
}

ZTEST(touch_state, test_negative_and_nonmonotonic_times_are_rejected)
{
    codex_touch_edge(true, -1);
    codex_connection_tick(-1);
    edge(true, 100);
    codex_touch_edge(false, 99);
    drain();
    tick(3100);

    zassert_true(codex_connection_mode_active());
    struct codex_touch_diagnostics diagnostics = codex_touch_diagnostics_get();
    zassert_equal(diagnostics.invalid_time, 3U);
}

ZTEST(touch_state, test_api_errors_keep_choice_and_indicator_consistent)
{
    enter_mode(0);
    fake_endpoint_select_error = -EIO;
    tap(4000, 4100);
    zassert_equal(codex_connection_choice_get(), CODEX_CONNECTION_BLE_1);
    zassert_equal(fake_active_profile, 0U);
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_TOP);

    fake_endpoint_select_error = 0;
    fake_profile_select_error = -EIO;
    tap(5000, 5100);
    zassert_equal(codex_connection_choice_get(), CODEX_CONNECTION_BLE_1);
    zassert_equal(fake_active_profile, 0U);
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_TOP);

    codex_touch_test_reset();
    fake_layer_activate_error = -EIO;
    tap(10000, 10100);
    zassert_equal(__wrap_zmk_keymap_highest_layer_active(), 1U);
    zassert_equal(codex_indicator_bits(), CODEX_INDICATOR_MIDDLE);
}

ZTEST(touch_state, test_full_queue_is_counted_without_blocking_callback)
{
    zassert_true(k_work_submit(&touch_blocker) >= 0);
    zassert_ok(k_sem_take(&touch_blocker_entered, K_MSEC(100)));
    for (size_t i = 0U; i < CONFIG_CODEX_TOUCH_QUEUE_DEPTH; i++) {
        codex_touch_edge((i & 1U) == 0U, (int64_t)i);
    }
    bool rejected_level = (CONFIG_CODEX_TOUCH_QUEUE_DEPTH & 1U) == 0U;
    codex_touch_edge(rejected_level, CONFIG_CODEX_TOUCH_QUEUE_DEPTH);
    codex_touch_edge(rejected_level, CONFIG_CODEX_TOUCH_QUEUE_DEPTH + 1U);
    k_sem_give(&touch_blocker_release);
    drain();

    struct codex_touch_diagnostics diagnostics = codex_touch_diagnostics_get();
    zassert_equal(diagnostics.queue_full, 2U);
}

ZTEST_SUITE(touch_state, NULL, NULL, reset_touch, NULL, NULL);
