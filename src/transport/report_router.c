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
};

K_MUTEX_DEFINE(router_lock);
K_MSGQ_DEFINE(ingress_msgq, sizeof(struct ingress_item),
              CODEX_ROUTER_INGRESS_DEPTH, 4);

static struct codex_route_state route_state;
static struct codex_router_diagnostics diagnostics;
static atomic_t ingress_busy;
static int callback_result;

static uint32_t *generation_for(enum codex_transport transport)
{
    return transport == CODEX_TRANSPORT_USB ? &route_state.usb_generation
                                            : &route_state.ble_generation;
}

static void invalidate_transport_locked(enum codex_transport transport)
{
    ++*generation_for(transport);
    codex_framing_reset_transport(transport);
    k_msgq_purge(&ingress_msgq);
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
    bool ready = state == ZMK_USB_CONN_HID;
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

        return route_state.ble_connected &&
               route_state.ble_profile == pin->ble_profile &&
               codex_ble_hids_token_is_current(&token, route_state.ble_profile);
    }
    return route_state.usb_hid_ready;
}

static int capture_route_locked(struct pinned_route *pin)
{
    if (!route_state.has_active) {
        return -ENOTCONN;
    }
    *pin = (struct pinned_route){
        .source = route_state.active,
        .route_generation = *generation_for(route_state.active),
        .ble_profile = route_state.ble_profile,
        .ble_hids_generation = codex_ble_hids_generation(),
    };
    return pin_is_current_locked(pin) ? 0 : -ENOTCONN;
}

static int emit_pinned(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE], void *context)
{
    const struct pinned_route *pin = context;
    int err;

    if (!pin_is_current_locked(pin)) {
        return -ESTALE;
    }
    err = pin->source == CODEX_TRANSPORT_USB
              ? codex_usb_send_vendor(payload)
              : codex_ble_vendor_notify(payload);
    if (err != 0) {
        diagnostics.emit_errors++;
    }
    return err;
}

static int send_json_pinned_locked(const struct pinned_route *pin,
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
    struct pinned_route pin;
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
    return err;
}

int codex_router_send_json(enum codex_channel channel, const uint8_t *json,
                           size_t len)
{
    struct pinned_route pin;
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
    return err;
}

static int rpc_response_emit(enum codex_transport source, const uint8_t *json,
                             size_t len, void *context)
{
    const struct pinned_route *pin = context;

    if (source != pin->source) {
        return -ESTALE;
    }
    return send_json_pinned_locked(pin, CODEX_CHANNEL_RPC, json, len);
}

static void framed_json_received(enum codex_transport source,
                                 enum codex_channel channel,
                                 const uint8_t *json, size_t len)
{
    struct pinned_route pin;

    if (channel == CODEX_CHANNEL_DEBUG) {
        diagnostics.debug_messages++;
        return;
    }
    callback_result = capture_route_locked(&pin);
    if (callback_result == 0 && pin.source != source) {
        callback_result = -ESTALE;
    }
    if (callback_result == 0) {
        callback_result = codex_rpc_dispatch(source, json, len,
                                             rpc_response_emit, &pin);
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
    int err;

    k_mutex_lock(&router_lock, K_FOREVER);
    if (!item_is_current_locked(item)) {
        diagnostics.stale_input++;
        k_mutex_unlock(&router_lock);
        return -ESTALE;
    }
    callback_result = 0;
    err = codex_framing_ingest(item->source, item->payload,
                               item->route_generation, framed_json_received);
    if (err != 0) {
        diagnostics.framing_errors++;
    } else if (callback_result != 0) {
        err = callback_result;
    }
    k_mutex_unlock(&router_lock);
    return err;
}

static int process_one(void)
{
    struct ingress_item item;
    int err = k_msgq_get(&ingress_msgq, &item, K_NO_WAIT);

    return err == 0 ? process_item(&item) : err;
}

static void ingress_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    while (process_one() != -ENOMSG) {
    }
}
K_WORK_DEFINE(ingress_work, ingress_work_handler);

static int enqueue_locked(const struct ingress_item *item)
{
    int err = k_msgq_put(&ingress_msgq, item, K_NO_WAIT);

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
    if (!route_state.has_active || route_state.active != CODEX_TRANSPORT_USB ||
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

void codex_usb_vendor_payload_received(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    (void)codex_router_ingest_usb(payload);
}

void codex_ble_vendor_output_received_with_token(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
    const struct codex_ble_source_token *token)
{
    (void)codex_router_ingest_ble(payload, token);
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
    k_msgq_purge(&ingress_msgq);
    codex_framing_reset_transport(CODEX_TRANSPORT_USB);
    codex_framing_reset_transport(CODEX_TRANSPORT_BLE);
    k_mutex_unlock(&router_lock);
}

int codex_router_test_process_one(void) { return process_one(); }

void codex_router_test_set_generations(uint32_t usb_generation,
                                       uint32_t ble_generation)
{
    k_mutex_lock(&router_lock, K_FOREVER);
    route_state.usb_generation = usb_generation;
    route_state.ble_generation = ble_generation;
    k_mutex_unlock(&router_lock);
}
#endif
