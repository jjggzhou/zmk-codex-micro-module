#include "router_fakes.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>

#include <codex/ble_hids.h>
#include <codex/descriptor.h>
#include <codex/usb.h>

#define MAX_PACKETS 32U

struct bt_conn {
    uint8_t id;
    size_t references;
};

static struct bt_conn ble_connections[2] = {{.id = 0U}, {.id = 1U}};

static uint8_t usb_packets[MAX_PACKETS][CODEX_VENDOR_PAYLOAD_SIZE];
static uint8_t ble_packets[MAX_PACKETS][CODEX_VENDOR_PAYLOAD_SIZE];
static size_t usb_count;
static size_t ble_count;
static int send_error;
static enum codex_transport standard_transport;
static enum codex_transport mouse_release_transport;
static char clear_log[32];
static size_t clear_log_len;
static uint32_t ble_generation;
static size_t ble_purge_count;
static size_t ble_abort_count;
static size_t ble_fail_after;
static int ble_fail_error;
static size_t ble_notify_invocations;
static size_t ble_switch_on_notify;
static uint8_t ble_active_connection;
static size_t ble_notify_by_connection[2];
static size_t ble_disconnect_by_connection[2];
static size_t ble_peak_references[2];
static size_t usb_fail_after;
static int usb_fail_error;
static size_t usb_attempts;
static size_t usb_successes_in_session;
static size_t usb_session;
static size_t usb_packet_sessions[MAX_PACKETS];
static size_t usb_disable_count;
static size_t usb_enable_count;
static size_t usb_enable_failures;
static int usb_enable_error;
static bool usb_powered;
static uint8_t status_profile;
static uint8_t status_layer;
static uint8_t status_battery;
static bool block_first_usb_send;
K_SEM_DEFINE(usb_send_entered, 0, 1);
K_SEM_DEFINE(usb_send_release, 0, 1);
K_SEM_DEFINE(usb_attempted, 0, MAX_PACKETS);
K_SEM_DEFINE(usb_disabled, 0, MAX_PACKETS);
K_SEM_DEFINE(usb_enabled, 0, MAX_PACKETS);

extern void __wrap_zmk_hid_mouse_clear(void);

static void log_char(char value)
{
    if (clear_log_len + 1U < sizeof(clear_log)) {
        clear_log[clear_log_len++] = value;
        clear_log[clear_log_len] = '\0';
    }
}

void router_fakes_reset(void)
{
    memset(usb_packets, 0, sizeof(usb_packets));
    memset(ble_packets, 0, sizeof(ble_packets));
    usb_count = 0U;
    ble_count = 0U;
    send_error = 0;
    standard_transport = CODEX_TRANSPORT_BLE;
    mouse_release_transport = CODEX_TRANSPORT_COUNT;
    memset(clear_log, 0, sizeof(clear_log));
    clear_log_len = 0U;
    ble_generation = 0U;
    ble_purge_count = 0U;
    ble_abort_count = 0U;
    ble_fail_after = SIZE_MAX;
    ble_fail_error = 0;
    ble_notify_invocations = 0U;
    ble_switch_on_notify = SIZE_MAX;
    ble_active_connection = 0U;
    ble_connections[0].references = 0U;
    ble_connections[1].references = 0U;
    memset(ble_notify_by_connection, 0, sizeof(ble_notify_by_connection));
    memset(ble_disconnect_by_connection, 0, sizeof(ble_disconnect_by_connection));
    memset(ble_peak_references, 0, sizeof(ble_peak_references));
    usb_fail_after = SIZE_MAX;
    usb_fail_error = 0;
    usb_attempts = 0U;
    usb_successes_in_session = 0U;
    usb_session = 0U;
    memset(usb_packet_sessions, 0, sizeof(usb_packet_sessions));
    usb_disable_count = 0U;
    usb_enable_count = 0U;
    usb_enable_failures = 0U;
    usb_enable_error = 0;
    usb_powered = false;
    status_profile = 0U;
    status_layer = 0U;
    status_battery = 80U;
    block_first_usb_send = false;
    k_sem_reset(&usb_send_entered);
    k_sem_reset(&usb_send_release);
    k_sem_reset(&usb_attempted);
    k_sem_reset(&usb_disabled);
    k_sem_reset(&usb_enabled);
}

int codex_usb_send_vendor(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    usb_attempts++;
    k_sem_give(&usb_attempted);
    if (send_error != 0) {
        return send_error;
    }
    if (usb_successes_in_session >= usb_fail_after) {
        return usb_fail_error;
    }
    if (usb_count == MAX_PACKETS) {
        return -ENOSPC;
    }
    if (block_first_usb_send && usb_count == 0U) {
        k_sem_give(&usb_send_entered);
        (void)k_sem_take(&usb_send_release, K_FOREVER);
    }
    usb_packet_sessions[usb_count] = usb_session;
    memcpy(usb_packets[usb_count++], payload, CODEX_VENDOR_PAYLOAD_SIZE);
    usb_successes_in_session++;
    return 0;
}

int codex_ble_hids_capture_active(struct bt_conn **connection,
                                  struct codex_ble_source_token *token)
{
    struct bt_conn *active = &ble_connections[ble_active_connection];

    active->references++;
    if (active->references > ble_peak_references[active->id]) {
        ble_peak_references[active->id] = active->references;
    }
    *connection = active;
    *token = (struct codex_ble_source_token){status_profile, ble_generation};
    return 0;
}

void codex_ble_hids_release_connection(struct bt_conn *connection)
{
    if (connection == NULL) {
        return;
    }
    __ASSERT_NO_MSG(connection->references > 0U);
    connection->references--;
}

bool codex_ble_hids_connection_is_current(
    struct bt_conn *connection, const struct codex_ble_source_token *token)
{
    return connection == &ble_connections[ble_active_connection] &&
           token != NULL && token->profile_index == status_profile &&
           token->connection_generation == ble_generation;
}

int codex_ble_vendor_notify(
    struct bt_conn *connection, const struct codex_ble_source_token *token,
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE])
{
    ble_notify_invocations++;
    if (ble_notify_invocations == ble_switch_on_notify) {
        ble_active_connection = 1U;
    }
    if (!codex_ble_hids_connection_is_current(connection, token)) {
        return -ESTALE;
    }
    if (send_error != 0) {
        return send_error;
    }
    if (ble_count >= ble_fail_after) {
        return ble_fail_error;
    }
    if (ble_count == MAX_PACKETS) {
        return -ENOSPC;
    }
    ble_notify_by_connection[connection->id]++;
    memcpy(ble_packets[ble_count++], payload, CODEX_VENDOR_PAYLOAD_SIZE);
    return 0;
}

void codex_ble_hids_purge_queues(void)
{
    ble_generation++;
    ble_purge_count++;
}

int codex_ble_hids_abort_response(
    struct bt_conn *connection, const struct codex_ble_source_token *token)
{
    ble_abort_count++;
    if (!codex_ble_hids_connection_is_current(connection, token)) {
        return -ESTALE;
    }
    ble_disconnect_by_connection[connection->id]++;
    codex_ble_hids_purge_queues();
    return 0;
}

uint32_t codex_ble_hids_generation(void) { return ble_generation; }

bool codex_ble_hids_token_is_current(const struct codex_ble_source_token *token,
                                     uint8_t active_profile)
{
    status_profile = active_profile;
    return token != NULL && token->profile_index == active_profile &&
           token->connection_generation == ble_generation;
}

struct zmk_endpoint_instance zmk_endpoints_selected(void)
{
    struct zmk_endpoint_instance endpoint = {
        .transport = standard_transport == CODEX_TRANSPORT_USB
                         ? ZMK_TRANSPORT_USB
                         : ZMK_TRANSPORT_BLE,
    };
    endpoint.ble.profile_index = status_profile;
    return endpoint;
}

int zmk_endpoints_select_transport(enum zmk_transport transport)
{
    enum codex_transport next = transport == ZMK_TRANSPORT_USB
                                    ? CODEX_TRANSPORT_USB
                                    : CODEX_TRANSPORT_BLE;

    if (next == standard_transport) {
        return 0;
    }
    /* Mirror pinned endpoints.c clear_current() ordering. */
    log_char('K');
    log_char('C');
    __wrap_zmk_hid_mouse_clear();
    log_char('k');
    log_char('c');
    standard_transport = next;
    return 0;
}

void zmk_hid_mouse_clear(void) { log_char('M'); }

int zmk_endpoints_send_mouse_report(void)
{
    log_char('S');
    mouse_release_transport = standard_transport;
    return 0;
}

enum zmk_usb_conn_state zmk_usb_get_conn_state(void)
{
    return usb_powered ? ZMK_USB_CONN_POWERED : ZMK_USB_CONN_NONE;
}

bool zmk_usb_is_hid_ready(void) { return false; }

int __real_usb_enable(usb_dc_status_callback status_cb)
{
    ARG_UNUSED(status_cb);
    usb_enable_count++;
    k_sem_give(&usb_enabled);
    if (usb_enable_count <= usb_enable_failures) {
        return usb_enable_error;
    }
    return 0;
}

int usb_disable(void)
{
    usb_disable_count++;
    usb_session++;
    usb_successes_in_session = 0U;
    k_sem_give(&usb_disabled);
    return 0;
}

void codex_usb_transport_reset_writer(void) {}

int zmk_ble_active_profile_index(void) { return status_profile; }
bool zmk_ble_active_profile_is_connected(void) { return true; }
uint8_t zmk_keymap_highest_layer_active(void) { return status_layer; }
uint8_t zmk_battery_state_of_charge(void) { return status_battery; }

const struct zmk_event_type zmk_event_zmk_usb_conn_state_changed = {
    .name = "zmk_usb_conn_state_changed",
};

struct zmk_usb_conn_state_changed *
as_zmk_usb_conn_state_changed(const zmk_event_t *event)
{
    ARG_UNUSED(event);
    return NULL;
}

void router_fake_set_usb_powered(bool powered) { usb_powered = powered; }
void router_fake_set_send_error(int error) { send_error = error; }
void router_fake_fail_ble_after(size_t successful_packets, int error)
{
    ble_fail_after = successful_packets;
    ble_fail_error = error;
}
void router_fake_switch_ble_connection_on_notify(size_t invocation)
{
    ble_switch_on_notify = invocation;
}
size_t router_fake_ble_notify_count(uint8_t connection_id)
{
    return ble_notify_by_connection[connection_id];
}
size_t router_fake_ble_disconnect_count(uint8_t connection_id)
{
    return ble_disconnect_by_connection[connection_id];
}
size_t router_fake_ble_peak_references(uint8_t connection_id)
{
    return ble_peak_references[connection_id];
}
size_t router_fake_ble_references(uint8_t connection_id)
{
    return ble_connections[connection_id].references;
}
void router_fake_fail_usb_after(size_t successful_packets, int error)
{
    usb_fail_after = successful_packets;
    usb_fail_error = error;
}
void router_fake_clear_usb_failure(void)
{
    usb_fail_after = SIZE_MAX;
    usb_fail_error = 0;
}
int router_fake_wait_usb_attempts(size_t attempts, k_timeout_t timeout)
{
    while (usb_attempts < attempts) {
        int err = k_sem_take(&usb_attempted, timeout);
        if (err != 0) {
            return err;
        }
    }
    return 0;
}
size_t router_fake_usb_session(void) { return usb_session; }
size_t router_fake_usb_packet_session(size_t index) { return usb_packet_sessions[index]; }
size_t router_fake_usb_disable_count(void) { return usb_disable_count; }
size_t router_fake_usb_enable_count(void) { return usb_enable_count; }
int router_fake_wait_usb_disables(size_t count, k_timeout_t timeout)
{
    while (usb_disable_count < count) {
        int err = k_sem_take(&usb_disabled, timeout);
        if (err != 0) {
            return err;
        }
    }
    return 0;
}
int router_fake_wait_usb_enables(size_t count, k_timeout_t timeout)
{
    while (usb_enable_count < count) {
        int err = k_sem_take(&usb_enabled, timeout);
        if (err != 0) {
            return err;
        }
    }
    return 0;
}
void router_fake_fail_usb_enable(size_t failures, int error)
{
    usb_enable_failures = failures;
    usb_enable_error = error;
}
void router_fake_block_first_usb_send(bool block) { block_first_usb_send = block; }
int router_fake_wait_usb_send_entered(k_timeout_t timeout)
{
    return k_sem_take(&usb_send_entered, timeout);
}
void router_fake_release_usb_send(void) { k_sem_give(&usb_send_release); }
size_t router_fake_usb_count(void) { return usb_count; }
size_t router_fake_ble_count(void) { return ble_count; }
const uint8_t *router_fake_usb_packet(size_t index) { return usb_packets[index]; }
const uint8_t *router_fake_ble_packet(size_t index) { return ble_packets[index]; }
uint8_t router_fake_selected_profile(void) { return status_profile; }
enum codex_transport router_fake_standard_transport(void) { return standard_transport; }
const char *router_fake_clear_log(void) { return clear_log; }
enum codex_transport router_fake_mouse_release_transport(void)
{
    return mouse_release_transport;
}
uint32_t router_fake_ble_generation(void) { return ble_generation; }
size_t router_fake_ble_purge_count(void) { return ble_purge_count; }
size_t router_fake_ble_abort_count(void) { return ble_abort_count; }

void router_fake_set_status(uint8_t profile, uint8_t layer, uint8_t battery)
{
    status_profile = profile;
    status_layer = layer;
    status_battery = battery;
}
