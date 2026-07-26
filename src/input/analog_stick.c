#define DT_DRV_COMPAT codex_analog_stick

#include <codex/input.h>
#include <codex/transport.h>

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#define CODEX_ANALOG_JSON_MAX 64U
#define CODEX_ANALOG_SCALE 10000U
#define CODEX_ANALOG_TAU 6.28318530717958647692f

struct codex_analog_calibration_internal {
    int32_t center_x;
    int32_t center_y;
    int32_t max_x;
    int32_t max_y;
    int32_t dead_zone;
    bool invert_x;
    bool invert_y;
    uint16_t meaningful_delta;
    uint32_t refresh_interval_ms;
};

enum codex_analog_item_type {
    CODEX_ANALOG_ITEM_RAW,
    CODEX_ANALOG_ITEM_RADIAL,
    CODEX_ANALOG_ITEM_REFRESH,
};

struct codex_analog_item {
    enum codex_analog_item_type type;
    union {
        struct {
            int32_t x;
            int32_t y;
        } raw;
        struct codex_radial radial;
    } value;
};

struct codex_analog_state {
    struct codex_analog_calibration_internal calibration;
    const struct device *input_device;
    int32_t latest_x;
    int32_t latest_y;
    bool have_x;
    bool have_y;
    struct codex_radial last_noncenter;
    bool have_last_noncenter;
    bool center_sent;
    uint32_t last_emit_ms;
};

K_MSGQ_DEFINE(analog_queue, sizeof(struct codex_analog_item),
              CONFIG_CODEX_ANALOG_QUEUE_DEPTH, 4);
/* Sampled analog state is latest-wins: intermediate producer frames may be
 * coalesced, but a final center cannot be lost behind a blocked worker. */
static struct k_spinlock analog_mailbox_lock;
static struct codex_analog_item analog_mailbox;
static atomic_t analog_mailbox_pending;

static struct codex_analog_state default_state = {
    .calibration = {
        .center_x = 0,
        .center_y = 0,
        .max_x = 1,
        .max_y = 1,
        .meaningful_delta = CONFIG_CODEX_ANALOG_MEANINGFUL_DELTA,
        .refresh_interval_ms = CONFIG_CODEX_ANALOG_REFRESH_INTERVAL_MS,
    },
};
static struct codex_analog_state *active_state = &default_state;
static void refresh_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(refresh_work, refresh_work_handler);

#if defined(CONFIG_ZTEST)
static bool test_uptime_enabled;
static uint32_t test_uptime_ms;
#endif

static uint32_t analog_uptime_ms(void)
{
#if defined(CONFIG_ZTEST)
    if (test_uptime_enabled) {
        return test_uptime_ms;
    }
#endif
    return k_uptime_get_32();
}

static float normalize_axis(int32_t raw, int32_t center, int32_t maximum,
                            int32_t dead_zone, bool invert)
{
    int64_t delta = (int64_t)raw - center;
    int64_t limit = (int64_t)maximum - center;
    int64_t magnitude = delta < 0 ? -delta : delta;
    float normalized;

    if (magnitude <= dead_zone) {
        return 0.0f;
    }
    if (delta > 0) {
        normalized = (float)(delta - dead_zone) / (float)(limit - dead_zone);
    } else {
        normalized = (float)(delta + dead_zone) / (float)(limit - dead_zone);
    }
    normalized = CLAMP(normalized, -1.0f, 1.0f);
    return invert ? -normalized : normalized;
}

static struct codex_radial normalize_with(const struct codex_analog_calibration_internal *calibration,
                                          int32_t raw_x, int32_t raw_y)
{
    float x = normalize_axis(raw_x, calibration->center_x, calibration->max_x,
                             calibration->dead_zone, calibration->invert_x);
    float y = normalize_axis(raw_y, calibration->center_y, calibration->max_y,
                             calibration->dead_zone, calibration->invert_y);
    float distance = sqrtf(x * x + y * y);
    struct codex_radial radial;

    if (distance == 0.0f) {
        return (struct codex_radial){0.0f, 0.0f};
    }
    if (distance > 1.0f) {
        x /= distance;
        y /= distance;
        distance = 1.0f;
    }
    radial.angle = atan2f(y, x) / CODEX_ANALOG_TAU;
    if (radial.angle < 0.0f) {
        radial.angle += 1.0f;
    }
    radial.distance = distance;
    return radial;
}

static bool radial_is_valid(struct codex_radial value)
{
    return isfinite(value.angle) && isfinite(value.distance) && value.angle >= 0.0f &&
           value.angle < 1.0f && value.distance >= 0.0f && value.distance <= 1.0f;
}

static uint32_t scaled_unit(float value)
{
    uint32_t scaled = (uint32_t)(value * (float)CODEX_ANALOG_SCALE + 0.5f);

    return MIN(scaled, CODEX_ANALOG_SCALE);
}

static size_t append_unit(char *buffer, size_t buffer_size, uint32_t scaled)
{
    char fraction[5];
    size_t digits = 4U;
    int written;

    if (scaled == 0U) {
        written = snprintk(buffer, buffer_size, "0");
        return written > 0 ? (size_t)written : 0U;
    }
    if (scaled >= CODEX_ANALOG_SCALE) {
        written = snprintk(buffer, buffer_size, "1");
        return written > 0 ? (size_t)written : 0U;
    }
    (void)snprintk(fraction, sizeof(fraction), "%04u", scaled);
    while (digits > 0U && fraction[digits - 1U] == '0') {
        digits--;
    }
    written = snprintk(buffer, buffer_size, "0.%.*s", (int)digits, fraction);
    return written > 0 ? (size_t)written : 0U;
}

static int format_radial_json(char json[CODEX_ANALOG_JSON_MAX], struct codex_radial value)
{
    const char *prefix = "{\"m\":\"v.oai.rad\",\"p\":{\"a\":";
    const char *middle = ",\"d\":";
    const char *suffix = "}}";
    size_t used = 0U;
    size_t written;

    if (value.distance == 0.0f) {
        int length = snprintk(json, CODEX_ANALOG_JSON_MAX,
                              "{\"m\":\"v.oai.rad\",\"p\":{\"a\":0,\"d\":0}}");

        return length >= 0 && length < CODEX_ANALOG_JSON_MAX ? length : -EINVAL;
    }
    written = strlen(prefix);
    if (written >= CODEX_ANALOG_JSON_MAX) {
        return -EINVAL;
    }
    memcpy(json + used, prefix, written);
    used += written;
    written = append_unit(json + used, CODEX_ANALOG_JSON_MAX - used, scaled_unit(value.angle));
    if (written == 0U || used + written >= CODEX_ANALOG_JSON_MAX) {
        return -EINVAL;
    }
    used += written;
    written = strlen(middle);
    memcpy(json + used, middle, written);
    used += written;
    written = append_unit(json + used, CODEX_ANALOG_JSON_MAX - used, scaled_unit(value.distance));
    if (written == 0U || used + written >= CODEX_ANALOG_JSON_MAX) {
        return -EINVAL;
    }
    used += written;
    written = strlen(suffix);
    if (used + written >= CODEX_ANALOG_JSON_MAX) {
        return -EINVAL;
    }
    memcpy(json + used, suffix, written + 1U);
    return (int)(used + written);
}

static float radial_delta(struct codex_radial left, struct codex_radial right)
{
    float left_x = cosf(left.angle * CODEX_ANALOG_TAU) * left.distance;
    float left_y = sinf(left.angle * CODEX_ANALOG_TAU) * left.distance;
    float right_x = cosf(right.angle * CODEX_ANALOG_TAU) * right.distance;
    float right_y = sinf(right.angle * CODEX_ANALOG_TAU) * right.distance;

    return sqrtf((left_x - right_x) * (left_x - right_x) +
                 (left_y - right_y) * (left_y - right_y));
}

static bool refresh_elapsed(uint32_t now, uint32_t previous, uint32_t interval)
{
    return (uint32_t)(now - previous) >= interval;
}

static void process_radial(struct codex_analog_state *state, struct codex_radial value,
                           bool forced_refresh)
{
    char json[CODEX_ANALOG_JSON_MAX];
    uint32_t now = analog_uptime_ms();
    bool emit = false;
    int length;

    if (!radial_is_valid(value)) {
        return;
    }
    if (value.distance == 0.0f) {
        if (state->center_sent) {
            return;
        }
        state->center_sent = true;
        state->have_last_noncenter = false;
        (void)k_work_cancel_delayable(&refresh_work);
        emit = true;
    } else {
        uint32_t threshold = state->calibration.meaningful_delta;

        state->center_sent = false;
        emit = forced_refresh || !state->have_last_noncenter ||
               radial_delta(value, state->last_noncenter) >=
                   (float)threshold / (float)CODEX_ANALOG_SCALE ||
               refresh_elapsed(now, state->last_emit_ms,
                               state->calibration.refresh_interval_ms);
        if (emit) {
            state->last_noncenter = value;
            state->have_last_noncenter = true;
        }
    }
    if (!emit) {
        return;
    }
    length = format_radial_json(json, value);
    if (length < 0) {
        return;
    }
    state->last_emit_ms = now;
    /* Like the key worker, each owned item is consumed even on router errors. */
    (void)codex_router_send_json(CODEX_CHANNEL_RPC, (const uint8_t *)json, (size_t)length);
    if (value.distance > 0.0f) {
        (void)k_work_reschedule(&refresh_work,
                                K_MSEC(state->calibration.refresh_interval_ms));
    }
}

static bool process_one(void)
{
    struct codex_analog_item item;
    struct codex_radial radial;

    if (k_msgq_get(&analog_queue, &item, K_NO_WAIT) != 0) {
        return false;
    }
    if (item.type == CODEX_ANALOG_ITEM_RAW) {
        radial = normalize_with(&active_state->calibration, item.value.raw.x, item.value.raw.y);
    } else if (item.type == CODEX_ANALOG_ITEM_RADIAL) {
        radial = item.value.radial;
    } else {
        if (!active_state->have_last_noncenter) {
            return true;
        }
        radial = active_state->last_noncenter;
    }
    process_radial(active_state, radial, item.type == CODEX_ANALOG_ITEM_REFRESH);
    return true;
}

static bool process_mailbox(void)
{
    struct codex_analog_item item;
    k_spinlock_key_t key = k_spin_lock(&analog_mailbox_lock);

    if (!atomic_get(&analog_mailbox_pending)) {
        k_spin_unlock(&analog_mailbox_lock, key);
        return false;
    }
    item = analog_mailbox;
    atomic_clear(&analog_mailbox_pending);
    k_spin_unlock(&analog_mailbox_lock, key);
    process_radial(active_state,
                   normalize_with(&active_state->calibration, item.value.raw.x, item.value.raw.y),
                   false);
    return true;
}

static void analog_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    while (process_one()) {
    }
    while (process_mailbox()) {
    }
}

K_WORK_DEFINE(analog_work, analog_work_handler);

static void refresh_work_handler(struct k_work *work)
{
    const struct codex_analog_item item = {.type = CODEX_ANALOG_ITEM_REFRESH};

    ARG_UNUSED(work);
    if (active_state->have_last_noncenter &&
        k_msgq_put(&analog_queue, &item, K_NO_WAIT) == 0) {
        (void)k_work_submit(&analog_work);
    } else if (active_state->have_last_noncenter) {
        (void)k_work_reschedule(&refresh_work,
                                K_MSEC(active_state->calibration.refresh_interval_ms));
    }
}

static int queue_item(const struct codex_analog_item *item)
{
    if (k_msgq_put(&analog_queue, item, K_NO_WAIT) != 0) {
        return -ENOSPC;
    }
    (void)k_work_submit(&analog_work);
    return 0;
}

int codex_input_radial_emit(struct codex_radial value)
{
    const struct codex_analog_item item = {
        .type = CODEX_ANALOG_ITEM_RADIAL,
        .value.radial = value,
    };

    return radial_is_valid(value) ? queue_item(&item) : -EINVAL;
}

static int analog_input_event(struct codex_analog_state *state, const struct input_event *event)
{
    struct codex_analog_item item;

    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return -EINVAL;
    }
    if (event->code == INPUT_REL_X) {
        state->latest_x = event->value;
        state->have_x = true;
    } else {
        state->latest_y = event->value;
        state->have_y = true;
    }
    if (!event->sync || !state->have_x || !state->have_y) {
        return 0;
    }
    item = (struct codex_analog_item){
        .type = CODEX_ANALOG_ITEM_RAW,
        .value.raw = {.x = state->latest_x, .y = state->latest_y},
    };
    k_spinlock_key_t key = k_spin_lock(&analog_mailbox_lock);
    analog_mailbox = item;
    atomic_set(&analog_mailbox_pending, 1);
    k_spin_unlock(&analog_mailbox_lock, key);
    (void)k_work_submit(&analog_work);
    return 0;
}

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "Codex supports one analog-stick adapter");
#define CODEX_SOURCE_AXIS_VALID(axis, code) \
    DT_SAME_NODE(DT_PARENT(DT_INST_PHANDLE(0, source_##axis##_channel)), \
                 DT_INST_PHANDLE(0, input_device)) && \
    DT_NODE_HAS_COMPAT(DT_PARENT(DT_INST_PHANDLE(0, source_##axis##_channel)), zmk_analog_input) && \
    DT_PROP(DT_INST_PHANDLE(0, source_##axis##_channel), report_on_change_only) && \
    DT_PROP(DT_INST_PHANDLE(0, source_##axis##_channel), evt_type) == INPUT_EV_REL && \
    DT_PROP(DT_INST_PHANDLE(0, source_##axis##_channel), input_code) == code && \
    DT_PROP(DT_INST_PHANDLE(0, source_##axis##_channel), mv_mid) == 0 && \
    DT_PROP(DT_INST_PHANDLE(0, source_##axis##_channel), mv_deadzone) == 0 && \
    DT_PROP(DT_INST_PHANDLE(0, source_##axis##_channel), mv_min_max) == 0 && \
    DT_PROP(DT_INST_PHANDLE(0, source_##axis##_channel), scale_multiplier) == 1 && \
    DT_PROP(DT_INST_PHANDLE(0, source_##axis##_channel), scale_divisor) == 1 && \
    !DT_PROP(DT_INST_PHANDLE(0, source_##axis##_channel), invert)
BUILD_ASSERT(CODEX_SOURCE_AXIS_VALID(x, INPUT_REL_X), "analog X source must pass absolute mV");
BUILD_ASSERT(CODEX_SOURCE_AXIS_VALID(y, INPUT_REL_Y), "analog Y source must pass absolute mV");
BUILD_ASSERT(!DT_SAME_NODE(DT_INST_PHANDLE(0, source_x_channel),
                           DT_INST_PHANDLE(0, source_y_channel)),
             "analog X and Y source children must be distinct");
BUILD_ASSERT(DT_INST_PROP(0, max_x) > DT_INST_PROP(0, center_x),
             "codex,analog-stick max-x must exceed center-x");
BUILD_ASSERT(DT_INST_PROP(0, max_y) > DT_INST_PROP(0, center_y),
             "codex,analog-stick max-y must exceed center-y");
BUILD_ASSERT(DT_INST_PROP(0, dead_zone) >= 0 &&
                 (int64_t)DT_INST_PROP(0, dead_zone) <
                     (int64_t)DT_INST_PROP(0, max_x) - DT_INST_PROP(0, center_x) &&
                 (int64_t)DT_INST_PROP(0, dead_zone) <
                     (int64_t)DT_INST_PROP(0, max_y) - DT_INST_PROP(0, center_y),
             "codex,analog-stick dead-zone must fit both raw axis ranges");
BUILD_ASSERT(DT_INST_PROP_OR(0, meaningful_delta, CONFIG_CODEX_ANALOG_MEANINGFUL_DELTA) <=
                 CODEX_ANALOG_SCALE,
             "codex,analog-stick meaningful-delta must be 0..10000");
BUILD_ASSERT(DT_INST_PROP_OR(0, refresh_interval_ms, CONFIG_CODEX_ANALOG_REFRESH_INTERVAL_MS) > 0 &&
                 DT_INST_PROP_OR(0, refresh_interval_ms, CONFIG_CODEX_ANALOG_REFRESH_INTERVAL_MS) <=
                     INT32_MAX,
             "codex,analog-stick refresh-interval-ms must be 1..INT32_MAX");

static struct codex_analog_state dts_state = {
    .calibration = {
        .center_x = DT_INST_PROP(0, center_x),
        .center_y = DT_INST_PROP(0, center_y),
        .max_x = DT_INST_PROP(0, max_x),
        .max_y = DT_INST_PROP(0, max_y),
        .dead_zone = DT_INST_PROP(0, dead_zone),
        .invert_x = DT_INST_PROP(0, invert_x),
        .invert_y = DT_INST_PROP(0, invert_y),
        .meaningful_delta = DT_INST_PROP_OR(0, meaningful_delta,
                                             CONFIG_CODEX_ANALOG_MEANINGFUL_DELTA),
        .refresh_interval_ms = DT_INST_PROP_OR(0, refresh_interval_ms,
                                                CONFIG_CODEX_ANALOG_REFRESH_INTERVAL_MS),
    },
    .input_device = DEVICE_DT_GET(DT_INST_PHANDLE(0, input_device)),
    .latest_x = DT_INST_PROP(0, center_x),
    .latest_y = DT_INST_PROP(0, center_y),
    .have_x = true,
    .have_y = true,
};

static void analog_input_callback(struct input_event *event)
{
    if (event->dev == dts_state.input_device) {
        active_state = &dts_state;
        (void)analog_input_event(&dts_state, event);
    }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_INST_PHANDLE(0, input_device)), analog_input_callback);
#endif

#if defined(CONFIG_ZTEST)
static void reset_analog_state(struct codex_analog_state *state)
{
    state->latest_x = state->calibration.center_x;
    state->latest_y = state->calibration.center_y;
    state->have_x = true;
    state->have_y = true;
    state->have_last_noncenter = false;
    state->center_sent = false;
    state->last_emit_ms = 0U;
}

struct codex_radial codex_analog_normalize(int32_t raw_x, int32_t raw_y)
{
    return normalize_with(&active_state->calibration, raw_x, raw_y);
}

int codex_analog_test_configure(const struct codex_analog_calibration *calibration)
{
    int64_t x_range;
    int64_t y_range;

    if (calibration == NULL) {
        return -EINVAL;
    }
    x_range = (int64_t)calibration->max_x - calibration->center_x;
    y_range = (int64_t)calibration->max_y - calibration->center_y;
    if (calibration->max_x <= calibration->center_x ||
        calibration->max_y <= calibration->center_y || calibration->dead_zone < 0 ||
        calibration->dead_zone >= x_range || calibration->dead_zone >= y_range ||
        calibration->meaningful_delta > CODEX_ANALOG_SCALE ||
        calibration->refresh_interval_ms == 0U || calibration->refresh_interval_ms > INT32_MAX) {
        return -EINVAL;
    }
    default_state.calibration = (struct codex_analog_calibration_internal){
        .center_x = calibration->center_x,
        .center_y = calibration->center_y,
        .max_x = calibration->max_x,
        .max_y = calibration->max_y,
        .dead_zone = calibration->dead_zone,
        .invert_x = calibration->invert_x,
        .invert_y = calibration->invert_y,
        .meaningful_delta = calibration->meaningful_delta,
        .refresh_interval_ms = calibration->refresh_interval_ms,
    };
    default_state.latest_x = calibration->center_x;
    default_state.latest_y = calibration->center_y;
    default_state.have_x = true;
    default_state.have_y = true;
    active_state = &default_state;
    return 0;
}

int codex_analog_test_input_event(uint16_t code, int32_t value, bool sync)
{
    const struct input_event event = {
        .type = INPUT_EV_REL,
        .code = code,
        .value = value,
        .sync = sync,
    };

    return analog_input_event(active_state, &event);
}

void codex_analog_test_reset(void)
{
    k_spinlock_key_t key = k_spin_lock(&analog_mailbox_lock);

    atomic_clear(&analog_mailbox_pending);
    k_spin_unlock(&analog_mailbox_lock, key);
    k_msgq_purge(&analog_queue);
    (void)k_work_cancel_delayable(&refresh_work);
    reset_analog_state(&default_state);
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
    reset_analog_state(&dts_state);
#endif
    active_state = &default_state;
    test_uptime_enabled = false;
}

void codex_analog_test_set_uptime(uint32_t uptime_ms)
{
    test_uptime_enabled = true;
    test_uptime_ms = uptime_ms;
}
#endif
