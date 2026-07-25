#include <codex/transport.h>

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/hid.h>
#include <zmk/usb.h>

#include <codex/rpc.h>
#include <codex/usb.h>

struct ingress_item {
    enum codex_transport source;
    uint32_t route_generation;
    struct codex_ble_source_token ble_token;
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];
};

struct pinned_route {
    enum codex_transport source;
    uint32_t route_generation;
    uint8_t ble_profile;
    uint32_t ble_hids_generation;
    struct bt_conn *ble_connection;
    uint16_t emitted_reports;
    bool abort_ble;
    bool recover_usb;
};

enum usb_recovery_state {
    USB_RECOVERY_IDLE,
    USB_RECOVERY_DETACH_PENDING,
    USB_RECOVERY_REENABLE_PENDING,
};

K_MUTEX_DEFINE(router_lock);
K_MSGQ_DEFINE(usb_ingress_msgq, sizeof(struct ingress_item),
              CODEX_ROUTER_INGRESS_DEPTH, 4);
K_MSGQ_DEFINE(ble_ingress_msgq, sizeof(struct ingress_item),
              CODEX_ROUTER_INGRESS_DEPTH, 4);

static struct codex_route_state route_state;
static struct codex_router_diagnostics diagnostics;
static atomic_t ingress_busy;
static int callback_result;
static struct pinned_route callback_pin;
static bool callback_pin_owned;
static atomic_t usb_recovery_state;
static void codex_usb_status_callback(enum usb_dc_status_code status,
                                      const uint8_t *params);
extern int __real_usb_enable(usb_dc_status_callback status_cb);

static void usb_recovery_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(usb_recovery_work, usb_recovery_work_handler);

static uint32_t *generation_for(enum codex_transport transport)
{
    return transport == CODEX_TRANSPORT_USB ? &route_state.usb_generation
                                            : &route_state.ble_generation;
}

static void invalidate_transport_locked(enum codex_transport transport)
{
    ++*generation_for(transport);
    codex_framing_reset_transport(transport);
    k_msgq_purge(transport == CODEX_TRANSPORT_USB ? &usb_ingress_msgq
                                                  : &ble_ingress_msgq);
}

static void select_standard_endpoint_locked(enum codex_transport next)
{
    enum zmk_transport zmk_next = next == CODEX_TRANSPORT_USB
                                      ? ZMK_TRANSPORT_USB
                                      : ZMK_TRANSPORT_BLE;

    (void)zmk_endpoints_select_transport(zmk_next);
}

#if IS_ENABLED(CONFIG_ZMK_POINTING)
extern void __real_zmk_hid_mouse_clear(void);

void __wrap_zmk_hid_mouse_clear(void)
{
    __real_zmk_hid_mouse_clear();
    /* endpoints.c calls this before changing current_instance, so the zero
     * mouse report is delivered exactly once to the old endpoint. */
    (void)zmk_endpoints_send_mouse_report();
}
#endif

static bool update_route_locked(bool usb_already_invalidated,
                                bool ble_already_invalidated)
{
    bool next_available = route_state.usb_hid_ready || route_state.ble_connected;
    enum codex_transport next = route_state.usb_hid_ready
                                    ? CODEX_TRANSPORT_USB
                                    : CODEX_TRANSPORT_BLE;
    bool purge_ble = false;

    if (route_state.has_active == next_available &&
        (!next_available || route_state.active == next)) {
        return false;
    }

    if (route_state.has_active) {
        if (route_state.active == CODEX_TRANSPORT_USB &&
            !usb_already_invalidated) {
            invalidate_transport_locked(CODEX_TRANSPORT_USB);
        } else if (route_state.active == CODEX_TRANSPORT_BLE &&
                   !ble_already_invalidated) {
            invalidate_transport_locked(CODEX_TRANSPORT_BLE);
            purge_ble = true;
        }
    }

    if (next_available) {
        select_standard_endpoint_locked(next);
        route_state.active = next;
    }
    route_state.has_active = next_available;
    return purge_ble;
}

void codex_router_on_usb_state(enum zmk_usb_conn_state state)
{
    bool ready = state == ZMK_USB_CONN_HID &&
                 atomic_get(&usb_recovery_state) == USB_RECOVERY_IDLE;
    bool invalidated = false;
    bool purge_ble;

    k_mutex_lock(&router_lock, K_FOREVER);
    if (route_state.usb_hid_ready != ready) {
        route_state.usb_hid_ready = ready;
        invalidate_transport_locked(CODEX_TRANSPORT_USB);
        invalidated = true;
    }
    purge_ble = update_route_locked(invalidated, false);
    if (purge_ble) {
        codex_ble_hids_purge_queues();
    }
    k_mutex_unlock(&router_lock);
}

void codex_router_on_usb_physical_state(enum usb_dc_status_code status)
{
    if (status != USB_DC_DISCONNECTED && status != USB_DC_RESET &&
        status != USB_DC_CONFIGURED) {
        return;
    }
    k_mutex_lock(&router_lock, K_FOREVER);
    invalidate_transport_locked(CODEX_TRANSPORT_USB);
    k_mutex_unlock(&router_lock);
}

void codex_router_on_ble_profile(uint8_t profile, bool connected)
{
    bool changed;
    bool purge_ble;

    k_mutex_lock(&router_lock, K_FOREVER);
    changed = route_state.ble_profile != profile ||
              route_state.ble_connected != connected;
    route_state.ble_profile = profile;
    route_state.ble_connected = connected;
    if (changed) {
        invalidate_transport_locked(CODEX_TRANSPORT_BLE);
    }
    purge_ble = update_route_locked(false, changed);
    if (changed || purge_ble) {
        codex_ble_hids_purge_queues();
    }
    k_mutex_unlock(&router_lock);
}

void codex_router_on_ble_connection_edge(uint8_t profile, bool connected)
{
    k_mutex_lock(&router_lock, K_FOREVER);
    route_state.ble_profile = profile;
    route_state.ble_connected = connected;
    invalidate_transport_locked(CODEX_TRANSPORT_BLE);
    (void)update_route_locked(false, true);
    k_mutex_unlock(&router_lock);
}

struct codex_route_state codex_router_state(void)
{
    struct codex_route_state snapshot;

    k_mutex_lock(&router_lock, K_FOREVER);
    snapshot = route_state;
    k_mutex_unlock(&router_lock);
    return snapshot;
}

static bool pin_is_current_locked(const struct pinned_route *pin)
{
    if (!route_state.has_active || route_state.active != pin->source ||
        *generation_for(pin->source) != pin->route_generation) {
        return false;
    }
    if (pin->source == CODEX_TRANSPORT_BLE) {
        struct codex_ble_source_token token = {
            .profile_index = pin->ble_profile,
            .connection_generation = pin->ble_hids_generation,
        };

        return pin->ble_connection != NULL && route_state.ble_connected &&
               route_state.ble_profile == pin->ble_profile &&
               codex_ble_hids_token_is_current(&token, route_state.ble_profile) &&
               codex_ble_hids_connection_is_current(pin->ble_connection, &token);
    }
    return route_state.usb_hid_ready;
}

static int capture_route_locked(struct pinned_route *pin)
{
    int err;

    if (!route_state.has_active) {
        return -ENOTCONN;
    }
    *pin = (struct pinned_route){
        .source = route_state.active,
        .route_generation = *generation_for(route_state.active),
        .ble_profile = route_state.ble_profile,
        .ble_hids_generation = codex_ble_hids_generation(),
    };
    if (pin->source == CODEX_TRANSPORT_BLE) {
        struct codex_ble_source_token token;

        err = codex_ble_hids_capture_active(&pin->ble_connection, &token);
        if (err != 0) {
            return err;
        }
        pin->ble_profile = token.profile_index;
        pin->ble_hids_generation = token.connection_generation;
    }
    if (!pin_is_current_locked(pin)) {
        codex_ble_hids_release_connection(pin->ble_connection);
        pin->ble_connection = NULL;
        return -ENOTCONN;
    }
    return 0;
}

static void release_pin(struct pinned_route *pin)
{
    codex_ble_hids_release_connection(pin->ble_connection);
    pin->ble_connection = NULL;
}

static void finalize_pin(struct pinned_route *pin)
{
    if (pin->abort_ble) {
        const struct codex_ble_source_token token = {
            .profile_index = pin->ble_profile,
            .connection_generation = pin->ble_hids_generation,
        };

        (void)codex_ble_hids_abort_response(pin->ble_connection, &token);
    }
    release_pin(pin);
    if (pin->recover_usb) {
        (void)k_work_reschedule(&usb_recovery_work, K_NO_WAIT);
    }
}

static void request_usb_recovery_locked(struct pinned_route *pin)
{
    bool invalidated = false;

    if (atomic_cas(&usb_recovery_state, USB_RECOVERY_IDLE,
                   USB_RECOVERY_DETACH_PENDING)) {
        diagnostics.usb_recoveries++;
        route_state.usb_hid_ready = false;
        invalidate_transport_locked(CODEX_TRANSPORT_USB);
        invalidated = true;
        pin->recover_usb = true;
    }
    if (invalidated) {
        (void)update_route_locked(true, false);
    }
}

static int emit_pinned(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE], void *context)
{
    struct pinned_route *pin = context;
    int err;

    if (!pin_is_current_locked(pin)) {
        return -ESTALE;
    }
    if (pin->source == CODEX_TRANSPORT_USB) {
        err = codex_usb_send_vendor(payload);
    } else {
        const struct codex_ble_source_token token = {
            .profile_index = pin->ble_profile,
            .connection_generation = pin->ble_hids_generation,
        };
        err = codex_ble_vendor_notify(pin->ble_connection, &token, payload);
    }
    if (err != 0) {
        diagnostics.emit_errors++;
        if (pin->source == CODEX_TRANSPORT_BLE && pin->emitted_reports > 0U) {
            diagnostics.aborted_responses++;
            pin->abort_ble = true;
        } else if (pin->source == CODEX_TRANSPORT_USB &&
                   pin->emitted_reports > 0U) {
            diagnostics.aborted_responses++;
            request_usb_recovery_locked(pin);
        }
    } else {
        pin->emitted_reports++;
    }
    return err;
}

static int send_json_pinned_locked(struct pinned_route *pin,
                                   enum codex_channel channel,
                                   const uint8_t *json, size_t len)
{
    if (!pin_is_current_locked(pin)) {
        return -ESTALE;
    }
    return codex_framing_encode(channel, json, len, emit_pinned, (void *)pin);
}

int codex_router_send_vendor(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    struct pinned_route pin = {0};
    int err;

    if (payload == NULL) {
        return -EINVAL;
    }
    k_mutex_lock(&router_lock, K_FOREVER);
    err = capture_route_locked(&pin);
    if (err == 0) {
        err = emit_pinned(payload, &pin);
    }
    k_mutex_unlock(&router_lock);
    finalize_pin(&pin);
    return err;
}

int codex_router_send_json(enum codex_channel channel, const uint8_t *json,
                           size_t len)
{
    struct pinned_route pin = {0};
    int err;

    if (json == NULL || len == 0U) {
        return -EINVAL;
    }
    k_mutex_lock(&router_lock, K_FOREVER);
    err = capture_route_locked(&pin);
    if (err == 0) {
        err = send_json_pinned_locked(&pin, channel, json, len);
    }
    k_mutex_unlock(&router_lock);
    finalize_pin(&pin);
    return err;
}

static int rpc_response_emit(enum codex_transport source, const uint8_t *json,
                             size_t len, void *context)
{
    struct pinned_route *pin = context;

    if (source != pin->source) {
        return -ESTALE;
    }
    return send_json_pinned_locked(pin, CODEX_CHANNEL_RPC, json, len);
}

static void framed_json_received(enum codex_transport source,
                                 enum codex_channel channel,
                                 const uint8_t *json, size_t len)
{
    if (channel == CODEX_CHANNEL_DEBUG) {
        diagnostics.debug_messages++;
        return;
    }
    callback_result = capture_route_locked(&callback_pin);
    callback_pin_owned = callback_result == 0;
    if (callback_result == 0 && callback_pin.source != source) {
        callback_result = -ESTALE;
    }
    if (callback_result == 0) {
        callback_result = codex_rpc_dispatch(source, json, len,
                                             rpc_response_emit, &callback_pin);
    }
    if (callback_result != 0) {
        diagnostics.rpc_errors++;
    }
}

static bool item_is_current_locked(const struct ingress_item *item)
{
    if (!route_state.has_active || route_state.active != item->source ||
        *generation_for(item->source) != item->route_generation) {
        return false;
    }
    return item->source != CODEX_TRANSPORT_BLE ||
           (route_state.ble_connected &&
            codex_ble_hids_token_is_current(&item->ble_token,
                                             route_state.ble_profile));
}

static int process_item(const struct ingress_item *item)
{
    struct pinned_route completed_pin;
    bool completed_pin_owned;
    int err;

    k_mutex_lock(&router_lock, K_FOREVER);
    if (!item_is_current_locked(item)) {
        diagnostics.stale_input++;
        k_mutex_unlock(&router_lock);
        return -ESTALE;
    }
    callback_result = 0;
    callback_pin_owned = false;
    err = codex_framing_ingest(item->source, item->payload,
                               item->route_generation, framed_json_received);
    if (err != 0) {
        diagnostics.framing_errors++;
    } else if (callback_result != 0) {
        err = callback_result;
    }
    completed_pin_owned = callback_pin_owned;
    if (completed_pin_owned) {
        completed_pin = callback_pin;
        memset(&callback_pin, 0, sizeof(callback_pin));
        callback_pin_owned = false;
    }
    k_mutex_unlock(&router_lock);
    if (completed_pin_owned) {
        finalize_pin(&completed_pin);
    }
    return err;
}

static int process_one(void)
{
    struct ingress_item item;
    int err = k_msgq_get(&usb_ingress_msgq, &item, K_NO_WAIT);

    if (err != 0) {
        err = k_msgq_get(&ble_ingress_msgq, &item, K_NO_WAIT);
    }

    return err == 0 ? process_item(&item) : err;
}

static void ingress_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    while (process_one() != -ENOMSG) {
    }
}
K_WORK_DEFINE(ingress_work, ingress_work_handler);

#define CODEX_USB_DETACH_TIME K_MSEC(10)
#define CODEX_USB_RECOVERY_RETRY_TIME K_MSEC(100)

static void record_usb_recovery_error(void)
{
    k_mutex_lock(&router_lock, K_FOREVER);
    diagnostics.usb_recovery_errors++;
    k_mutex_unlock(&router_lock);
}

static void usb_recovery_work_handler(struct k_work *work)
{
    int state = atomic_get(&usb_recovery_state);
    int err;

    ARG_UNUSED(work);
    if (state == USB_RECOVERY_DETACH_PENDING) {
        err = usb_disable();
        if (err != 0) {
            record_usb_recovery_error();
            (void)k_work_reschedule(&usb_recovery_work,
                                    CODEX_USB_RECOVERY_RETRY_TIME);
            return;
        }
        codex_usb_transport_reset_writer();
        atomic_set(&usb_recovery_state, USB_RECOVERY_REENABLE_PENDING);
        (void)k_work_reschedule(&usb_recovery_work, CODEX_USB_DETACH_TIME);
        return;
    }
    if (state == USB_RECOVERY_REENABLE_PENDING) {
        err = __real_usb_enable(codex_usb_status_callback);
        if (err != 0) {
            record_usb_recovery_error();
            (void)k_work_reschedule(&usb_recovery_work,
                                    CODEX_USB_RECOVERY_RETRY_TIME);
            return;
        }
        atomic_set(&usb_recovery_state, USB_RECOVERY_IDLE);
    }
}

static int enqueue_locked(const struct ingress_item *item)
{
    struct k_msgq *queue = item->source == CODEX_TRANSPORT_USB
                               ? &usb_ingress_msgq
                               : &ble_ingress_msgq;
    int err = k_msgq_put(queue, item, K_NO_WAIT);

    if (err != 0) {
        diagnostics.ingress_full++;
        return -ENOSPC;
    }
#if !defined(CONFIG_ZTEST)
    k_work_submit(&ingress_work);
#endif
    return 0;
}

int codex_router_ingest_usb(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    struct ingress_item item;
    int err;

    if (payload == NULL) {
        return -EINVAL;
    }
    if (k_mutex_lock(&router_lock, K_NO_WAIT) != 0) {
        atomic_inc(&ingress_busy);
        return -EAGAIN;
    }
    if (atomic_get(&usb_recovery_state) != USB_RECOVERY_IDLE) {
        diagnostics.stale_input++;
        err = -EAGAIN;
    } else if (!route_state.has_active || route_state.active != CODEX_TRANSPORT_USB ||
        !route_state.usb_hid_ready) {
        diagnostics.stale_input++;
        err = -ENOTCONN;
    } else {
        item = (struct ingress_item){
            .source = CODEX_TRANSPORT_USB,
            .route_generation = route_state.usb_generation,
        };
        memcpy(item.payload, payload, sizeof(item.payload));
        err = enqueue_locked(&item);
    }
    k_mutex_unlock(&router_lock);
    return err;
}

int codex_router_ingest_ble(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
    const struct codex_ble_source_token *token)
{
    struct ingress_item item;
    int err;

    if (payload == NULL || token == NULL) {
        return -EINVAL;
    }
    if (k_mutex_lock(&router_lock, K_NO_WAIT) != 0) {
        atomic_inc(&ingress_busy);
        return -EAGAIN;
    }
    if (!route_state.has_active || route_state.active != CODEX_TRANSPORT_BLE ||
        !route_state.ble_connected ||
        !codex_ble_hids_token_is_current(token, route_state.ble_profile)) {
        diagnostics.stale_input++;
        err = -ESTALE;
    } else {
        item = (struct ingress_item){
            .source = CODEX_TRANSPORT_BLE,
            .route_generation = route_state.ble_generation,
            .ble_token = *token,
        };
        memcpy(item.payload, payload, sizeof(item.payload));
        err = enqueue_locked(&item);
    }
    k_mutex_unlock(&router_lock);
    return err;
}

int codex_usb_vendor_payload_received(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    return codex_router_ingest_usb(payload);
}

int codex_ble_vendor_output_received_with_token(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
    const struct codex_ble_source_token *token)
{
    return codex_router_ingest_ble(payload, token);
}

static usb_dc_status_callback zmk_usb_status_callback;

static void codex_usb_status_callback(enum usb_dc_status_code status,
                                      const uint8_t *params)
{
    codex_router_on_usb_physical_state(status);
    if (zmk_usb_status_callback != NULL) {
        zmk_usb_status_callback(status, params);
    }
}

int __wrap_usb_enable(usb_dc_status_callback status_cb)
{
    zmk_usb_status_callback = status_cb;
    return __real_usb_enable(codex_usb_status_callback);
}

struct codex_router_diagnostics codex_router_diagnostics_snapshot(void)
{
    struct codex_router_diagnostics snapshot;

    k_mutex_lock(&router_lock, K_FOREVER);
    snapshot = diagnostics;
    snapshot.ingress_busy = (uint32_t)atomic_get(&ingress_busy);
    k_mutex_unlock(&router_lock);
    return snapshot;
}

static int usb_state_listener(const zmk_event_t *event)
{
    const struct zmk_usb_conn_state_changed *changed =
        as_zmk_usb_conn_state_changed(event);
    enum zmk_usb_conn_state state = changed->conn_state;

    if (state == ZMK_USB_CONN_HID && !zmk_usb_is_hid_ready()) {
        state = ZMK_USB_CONN_POWERED;
    }
    codex_router_on_usb_state(state);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(codex_router_usb, usb_state_listener);
ZMK_SUBSCRIPTION(codex_router_usb, zmk_usb_conn_state_changed);

#if IS_ENABLED(CONFIG_ZMK_BLE)
static int ble_profile_listener(const zmk_event_t *event)
{
    const struct zmk_ble_active_profile_changed *changed =
        as_zmk_ble_active_profile_changed(event);

    codex_router_on_ble_profile(changed->index,
                                zmk_ble_active_profile_is_connected());
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(codex_router_ble, ble_profile_listener);
ZMK_SUBSCRIPTION(codex_router_ble, zmk_ble_active_profile_changed);
#endif

#if defined(CONFIG_ZTEST)
void codex_router_test_reset(void)
{
    k_mutex_lock(&router_lock, K_FOREVER);
    memset(&route_state, 0, sizeof(route_state));
    memset(&diagnostics, 0, sizeof(diagnostics));
    atomic_clear(&ingress_busy);
    callback_result = 0;
    callback_pin_owned = false;
    memset(&callback_pin, 0, sizeof(callback_pin));
    (void)k_work_cancel_delayable(&usb_recovery_work);
    atomic_set(&usb_recovery_state, USB_RECOVERY_IDLE);
    k_msgq_purge(&usb_ingress_msgq);
    k_msgq_purge(&ble_ingress_msgq);
    codex_framing_reset_transport(CODEX_TRANSPORT_USB);
    codex_framing_reset_transport(CODEX_TRANSPORT_BLE);
    k_mutex_unlock(&router_lock);
}

int codex_router_test_process_one(void) { return process_one(); }

void codex_router_test_submit_work(void) { k_work_submit(&ingress_work); }

void codex_router_test_set_generations(uint32_t usb_generation,
                                       uint32_t ble_generation)
{
    k_mutex_lock(&router_lock, K_FOREVER);
    route_state.usb_generation = usb_generation;
    route_state.ble_generation = ble_generation;
    k_mutex_unlock(&router_lock);
}
#endif
