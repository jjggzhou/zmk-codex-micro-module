#include <codex/state.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "../state/state_internal.h"

enum touch_item_kind {
    TOUCH_ITEM_EDGE,
    TOUCH_ITEM_TICK,
};

struct touch_item {
    int64_t now_ms;
    enum touch_item_kind kind;
    bool touched;
};

static struct touch_item queue[CONFIG_CODEX_TOUCH_QUEUE_DEPTH];
static size_t queue_head;
static size_t queue_count;
static bool worker_scheduled;
static bool ingress_has_time;
static int64_t ingress_last_time;
static bool ingress_has_edge;
static bool ingress_edge_level;
static struct k_spinlock ingress_lock;

static bool touched;
static bool hold_handled;
static int64_t press_ms;
static int64_t last_activity_ms;
static int64_t last_processed_ms;
static enum codex_connection_choice connection_choice;
static bool connection_choice_initialized;
static atomic_t connection_mode;
static atomic_t connection_choice_snapshot;
static atomic_t queue_full_count;
static atomic_t invalid_time_count;

#if defined(CONFIG_ZTEST)
static int64_t test_uptime_ms;
static struct k_spinlock test_uptime_lock;
#endif

static void touch_work_handler(struct k_work *work);
static void deadline_work_handler(struct k_work *work);
K_WORK_DEFINE(touch_work, touch_work_handler);
K_WORK_DELAYABLE_DEFINE(deadline_work, deadline_work_handler);

static bool elapsed_at_least(int64_t now_ms, int64_t start_ms, uint32_t duration_ms)
{
    return now_ms >= start_ms && (uint64_t)now_ms - (uint64_t)start_ms >= duration_ms;
}

static uint32_t remaining_ms(int64_t now_ms, int64_t start_ms, uint32_t duration_ms)
{
    uint64_t elapsed = now_ms >= start_ms ? (uint64_t)now_ms - (uint64_t)start_ms : 0U;

    return elapsed >= duration_ms ? 0U : duration_ms - (uint32_t)elapsed;
}

static int64_t touch_uptime_get(void)
{
#if defined(CONFIG_ZTEST)
    k_spinlock_key_t key = k_spin_lock(&test_uptime_lock);
    int64_t now_ms = test_uptime_ms;

    k_spin_unlock(&test_uptime_lock, key);
    return now_ms;
#else
    return k_uptime_get();
#endif
}

static void submit_item(enum touch_item_kind kind, bool level, int64_t now_ms)
{
    bool submit = false;
    k_spinlock_key_t key;

    if (now_ms < 0) {
        atomic_inc(&invalid_time_count);
        return;
    }

    key = k_spin_lock(&ingress_lock);
    if ((ingress_has_time && now_ms < ingress_last_time) ||
        (kind == TOUCH_ITEM_EDGE && ingress_has_edge && level == ingress_edge_level)) {
        if (ingress_has_time && now_ms < ingress_last_time) {
            atomic_inc(&invalid_time_count);
        }
        k_spin_unlock(&ingress_lock, key);
        return;
    }
    if (queue_count == ARRAY_SIZE(queue)) {
        atomic_inc(&queue_full_count);
        k_spin_unlock(&ingress_lock, key);
        return;
    }

    queue[(queue_head + queue_count) % ARRAY_SIZE(queue)] = (struct touch_item){
        .now_ms = now_ms,
        .kind = kind,
        .touched = level,
    };
    queue_count++;
    ingress_has_time = true;
    ingress_last_time = now_ms;
    if (kind == TOUCH_ITEM_EDGE) {
        ingress_has_edge = true;
        ingress_edge_level = level;
    }
    if (!worker_scheduled) {
        worker_scheduled = true;
        submit = true;
    }
    k_spin_unlock(&ingress_lock, key);

    if (submit) {
        (void)k_work_submit(&touch_work);
    }
}

void codex_touch_edge(bool level, int64_t now_ms)
{
    submit_item(TOUCH_ITEM_EDGE, level, now_ms);
}

void codex_connection_tick(int64_t now_ms)
{
    submit_item(TOUCH_ITEM_TICK, false, now_ms);
}

static void enter_connection_mode(int64_t now_ms)
{
    if (!connection_choice_initialized || connection_choice != CODEX_CONNECTION_USB) {
        connection_choice = codex_connection_initial_choice();
    }
    connection_choice_initialized = true;
    atomic_set(&connection_choice_snapshot, connection_choice);
    atomic_set(&connection_mode, true);
    last_activity_ms = now_ms;
    codex_indicators_render(codex_connection_indicator_bits(connection_choice));
}

static void leave_connection_mode(void)
{
    atomic_set(&connection_mode, false);
    codex_indicators_render(codex_layers_current_indicator());
}

static void handle_hold(int64_t now_ms)
{
    if (hold_handled || !touched || !elapsed_at_least(now_ms, press_ms, CODEX_HOLD_MS)) {
        return;
    }

    hold_handled = true;
    if (!atomic_get(&connection_mode)) {
        enter_connection_mode(now_ms);
    } else {
        (void)codex_connection_clear_choice(connection_choice);
        last_activity_ms = now_ms;
        codex_indicators_render(codex_connection_indicator_bits(connection_choice));
    }
}

static void handle_edge(const struct touch_item *item)
{
    if (item->touched) {
        touched = true;
        hold_handled = false;
        press_ms = item->now_ms;
        if (atomic_get(&connection_mode)) {
            last_activity_ms = item->now_ms;
        }
        return;
    }

    if (!touched) {
        return;
    }
    handle_hold(item->now_ms);
    touched = false;
    if (atomic_get(&connection_mode)) {
        last_activity_ms = item->now_ms;
    }
    if (hold_handled) {
        return;
    }

    if (atomic_get(&connection_mode)) {
        enum codex_connection_choice requested =
            (enum codex_connection_choice)((connection_choice + 1U) %
                                           CODEX_CONNECTION_CHOICE_COUNT);
        enum codex_connection_choice actual = connection_choice;

        (void)codex_connection_apply_choice(requested, &actual);
        connection_choice = actual;
        atomic_set(&connection_choice_snapshot, connection_choice);
        codex_indicators_render(codex_connection_indicator_bits(connection_choice));
    } else {
        (void)codex_layers_cycle();
    }
}

static void handle_item(const struct touch_item *item)
{
    last_processed_ms = item->now_ms;
    if (item->kind == TOUCH_ITEM_EDGE) {
        handle_edge(item);
    } else {
        handle_hold(item->now_ms);
    }

    if (atomic_get(&connection_mode) &&
        elapsed_at_least(item->now_ms, last_activity_ms, CODEX_CONNECTION_IDLE_MS)) {
        leave_connection_mode();
    }
}

static bool pop_item(struct touch_item *item)
{
    k_spinlock_key_t key = k_spin_lock(&ingress_lock);

    if (queue_count == 0U) {
        worker_scheduled = false;
        k_spin_unlock(&ingress_lock, key);
        return false;
    }
    *item = queue[queue_head];
    queue_head = (queue_head + 1U) % ARRAY_SIZE(queue);
    queue_count--;
    k_spin_unlock(&ingress_lock, key);
    return true;
}

static void schedule_next_deadline(void)
{
    uint32_t delay_ms;

    if (touched && !hold_handled) {
        delay_ms = remaining_ms(last_processed_ms, press_ms, CODEX_HOLD_MS);
    } else if (atomic_get(&connection_mode)) {
        delay_ms = remaining_ms(last_processed_ms, last_activity_ms,
                                CODEX_CONNECTION_IDLE_MS);
    } else {
        (void)k_work_cancel_delayable(&deadline_work);
        return;
    }
    (void)k_work_reschedule(&deadline_work, K_MSEC(delay_ms));
}

static void touch_work_handler(struct k_work *work)
{
    struct touch_item item;

    ARG_UNUSED(work);
    while (pop_item(&item)) {
        handle_item(&item);
    }
    schedule_next_deadline();
}

static void deadline_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    codex_connection_tick(touch_uptime_get());
}

bool codex_connection_mode_active(void) { return atomic_get(&connection_mode) != 0; }

enum codex_connection_choice codex_connection_choice_get(void)
{
    return (enum codex_connection_choice)atomic_get(&connection_choice_snapshot);
}

struct codex_touch_diagnostics codex_touch_diagnostics_get(void)
{
    return (struct codex_touch_diagnostics){
        .queue_full = (uint32_t)atomic_get(&queue_full_count),
        .invalid_time = (uint32_t)atomic_get(&invalid_time_count),
    };
}

#if defined(CONFIG_ZTEST)
void codex_touch_test_drain(void)
{
    struct k_work_sync sync;

    (void)k_work_flush(&touch_work, &sync);
}

void codex_touch_test_set_uptime(int64_t now_ms)
{
    k_spinlock_key_t key = k_spin_lock(&test_uptime_lock);

    test_uptime_ms = now_ms;
    k_spin_unlock(&test_uptime_lock, key);
}

void codex_touch_test_fire_deadline(void)
{
    struct k_work_sync sync;

    (void)k_work_reschedule(&deadline_work, K_NO_WAIT);
    (void)k_work_flush_delayable(&deadline_work, &sync);
    codex_touch_test_drain();
}

void codex_touch_test_reset(void)
{
    struct k_work_sync sync;
    k_spinlock_key_t key;

    (void)k_work_cancel_delayable_sync(&deadline_work, &sync);
    codex_touch_test_drain();

    key = k_spin_lock(&ingress_lock);
    memset(queue, 0, sizeof(queue));
    queue_head = 0U;
    queue_count = 0U;
    worker_scheduled = false;
    ingress_has_time = false;
    ingress_last_time = 0;
    ingress_has_edge = false;
    ingress_edge_level = false;
    k_spin_unlock(&ingress_lock, key);

    touched = false;
    hold_handled = false;
    press_ms = 0;
    last_activity_ms = 0;
    last_processed_ms = 0;
    connection_choice = CODEX_CONNECTION_BLE_1;
    connection_choice_initialized = false;
    atomic_set(&connection_mode, false);
    atomic_set(&connection_choice_snapshot, CODEX_CONNECTION_BLE_1);
    atomic_set(&queue_full_count, 0);
    atomic_set(&invalid_time_count, 0);
    codex_touch_test_set_uptime(0);
    codex_indicators_reset();
}
#endif
