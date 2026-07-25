#include "router_fakes.h"

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <codex/transport.h>

static void make_fragment(uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
                          enum codex_channel channel, const char *json)
{
    size_t len = strlen(json);

    zassert_true(len <= CODEX_FRAGMENT_MAX);
    memset(payload, 0, CODEX_VENDOR_PAYLOAD_SIZE);
    payload[0] = channel;
    payload[1] = (uint8_t)len;
    memcpy(&payload[2], json, len);
}

static void process_ok(void) { zassert_ok(codex_router_test_process_one()); }

struct request_ingest_context {
    enum codex_transport transport;
    struct codex_ble_source_token token;
};

static int ingest_request_fragment(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE], void *context)
{
    struct request_ingest_context *ingest = context;
    int err = ingest->transport == CODEX_TRANSPORT_USB
                  ? codex_router_ingest_usb(payload)
                  : codex_router_ingest_ble(payload, &ingest->token);

    if (err == 0) {
        err = codex_router_test_process_one();
    }
    return err;
}

static int enqueue_usb_fragment(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE], void *context)
{
    ARG_UNUSED(context);
    return codex_router_ingest_usb(payload);
}

static bool usb_session_has_crlf(size_t session)
{
    uint8_t previous = 0U;

    for (size_t i = 0U; i < router_fake_usb_count(); i++) {
        const uint8_t *packet;
        size_t fragment_len;

        if (router_fake_usb_packet_session(i) != session) {
            continue;
        }
        packet = router_fake_usb_packet(i);
        fragment_len = packet[1];
        for (size_t j = 0U; j < fragment_len; j++) {
            uint8_t current = packet[2U + j];
            if (previous == '\r' && current == '\n') {
                return true;
            }
            previous = current;
        }
    }
    return false;
}

static int send_multifragment_usb_response(void)
{
    static const uint8_t json[] =
        "{\"result\":{\"text\":\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\"},\"id\":15}";

    return codex_router_send_json(CODEX_CHANNEL_RPC, json, sizeof(json) - 1U);
}

static void send_long_ble_request(bool standard)
{
    uint8_t request[420];
    size_t prefix_len;
    size_t suffix_len;
    struct request_ingest_context context = {
        .transport = CODEX_TRANSPORT_BLE,
        .token = {1U, router_fake_ble_generation()},
    };
    const char *prefix = standard
        ? "{\"jsonrpc\":\"2.0\",\"method\":\"sys.version\",\"params\":null,\"id\":\""
        : "{\"m\":\"sys.version\",\"p\":null,\"id\":\"";
    const char *suffix = "\"}";

    prefix_len = strlen(prefix);
    suffix_len = strlen(suffix);
    memcpy(request, prefix, prefix_len);
    memset(&request[prefix_len], 'x', 300U);
    memcpy(&request[prefix_len + 300U], suffix, suffix_len);
    zassert_ok(codex_framing_encode(CODEX_CHANNEL_RPC, request,
                                    prefix_len + 300U + suffix_len,
                                    ingest_request_fragment, &context));
}

static void collect_response(enum codex_transport transport, char *json,
                             size_t capacity)
{
    size_t count = transport == CODEX_TRANSPORT_USB ? router_fake_usb_count()
                                                    : router_fake_ble_count();
    size_t used = 0U;

    for (size_t i = 0U; i < count; i++) {
        const uint8_t *packet = transport == CODEX_TRANSPORT_USB
                                    ? router_fake_usb_packet(i)
                                    : router_fake_ble_packet(i);
        size_t fragment_len = packet[1];

        zassert_equal(packet[0], CODEX_CHANNEL_RPC);
        zassert_true(fragment_len <= CODEX_FRAGMENT_MAX);
        zassert_true(fragment_len < capacity - used);
        memcpy(&json[used], &packet[2], fragment_len);
        used += fragment_len;
    }
    json[used] = '\0';
}

static void router_before(void *fixture)
{
    ARG_UNUSED(fixture);
    router_fakes_reset();
    codex_router_test_reset();
}

ZTEST(router, test_ble2_powered_hid_none_restores_ble2_with_usb_priority)
{
    struct codex_route_state state;

    router_fake_set_status(2U, 0U, 80U);
    codex_router_on_ble_profile(2U, true);
    state = codex_router_state();
    zassert_true(state.has_active);
    zassert_equal(state.active, CODEX_TRANSPORT_BLE);

    router_fake_set_usb_powered(true);
    codex_router_on_usb_state(ZMK_USB_CONN_POWERED);
    zassert_equal(codex_router_state().active, CODEX_TRANSPORT_BLE);

    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    zassert_equal(codex_router_state().active, CODEX_TRANSPORT_USB);
    zassert_equal(router_fake_standard_transport(), CODEX_TRANSPORT_USB);
    zassert_equal(router_fake_mouse_release_transport(), CODEX_TRANSPORT_BLE);
    zassert_equal(strcmp(router_fake_clear_log(), "KCMSkc"), 0);

    codex_router_on_usb_state(ZMK_USB_CONN_NONE);
    state = codex_router_state();
    zassert_equal(state.active, CODEX_TRANSPORT_BLE);
    zassert_equal(state.ble_profile, 2U);
    zassert_equal(router_fake_selected_profile(), 2U);
}

static void round_trip(enum codex_transport transport, const char *request,
                       const char *response_needle)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];
    char response[CODEX_JSON_MAX_SIZE + 3U];

    make_fragment(payload, CODEX_CHANNEL_RPC, request);
    if (transport == CODEX_TRANSPORT_USB) {
        codex_router_on_usb_state(ZMK_USB_CONN_HID);
        zassert_ok(codex_router_ingest_usb(payload));
    } else {
        struct codex_ble_source_token token;
        codex_router_on_ble_profile(1U, true);
        token.profile_index = 1U;
        token.connection_generation = router_fake_ble_generation();
        zassert_ok(codex_router_ingest_ble(payload, &token));
    }
    process_ok();

    collect_response(transport, response, sizeof(response));
    zassert_not_null(strstr(response, response_needle));
}

ZTEST(router, test_full_compact_rpc_chain_over_usb)
{
    round_trip(CODEX_TRANSPORT_USB,
               "{\"m\":\"sys.version\",\"p\":null,\"id\":1}",
               "\"id\":1");
}

ZTEST(router, test_full_standard_rpc_chain_over_ble)
{
    round_trip(CODEX_TRANSPORT_BLE,
               "{\"jsonrpc\":\"2.0\",\"method\":\"sys.version\",\"params\":null,\"id\":2}",
               "\"id\":2");
}

ZTEST(router, test_full_standard_rpc_chain_over_usb)
{
    round_trip(CODEX_TRANSPORT_USB,
               "{\"jsonrpc\":\"2.0\",\"method\":\"sys.version\",\"params\":null,\"id\":4}",
               "\"id\":4");
}

ZTEST(router, test_full_compact_rpc_chain_over_ble)
{
    round_trip(CODEX_TRANSPORT_BLE,
               "{\"m\":\"sys.version\",\"p\":null,\"id\":5}",
               "\"id\":5");
}

ZTEST(router, test_compact_ble_response_can_exceed_four_reports)
{
    codex_router_on_ble_profile(1U, true);
    send_long_ble_request(false);
    zassert_true(router_fake_ble_count() > 4U);
}

ZTEST(router, test_standard_ble_response_can_exceed_four_reports)
{
    codex_router_on_ble_profile(1U, true);
    send_long_ble_request(true);
    zassert_true(router_fake_ble_count() > 4U);
}

ZTEST(router, test_debug_channel_never_enters_rpc)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];

    make_fragment(payload, CODEX_CHANNEL_DEBUG,
                  "{\"m\":\"sys.version\",\"p\":null,\"id\":1}");
    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    zassert_ok(codex_router_ingest_usb(payload));
    process_ok();
    zassert_equal(router_fake_usb_count(), 0U);
    zassert_equal(codex_router_diagnostics_snapshot().debug_messages, 1U);
}

ZTEST(router, test_queue_full_is_bounded_and_diagnosed)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];

    make_fragment(payload, CODEX_CHANNEL_DEBUG, "{}");
    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    for (size_t i = 0U; i < CODEX_ROUTER_INGRESS_DEPTH; i++) {
        zassert_ok(codex_router_ingest_usb(payload));
    }
    zassert_equal(codex_router_ingest_usb(payload), -ENOSPC);
    zassert_equal(codex_router_diagnostics_snapshot().ingress_full, 1U);
}

ZTEST(router, test_profile_and_usb_generation_changes_purge_stale_ingress)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];
    struct codex_ble_source_token token;

    make_fragment(payload, CODEX_CHANNEL_RPC,
                  "{\"m\":\"sys.version\",\"p\":null,\"id\":1}");
    codex_router_on_ble_profile(0U, true);
    token = (struct codex_ble_source_token){0U, router_fake_ble_generation()};
    zassert_ok(codex_router_ingest_ble(payload, &token));
    codex_router_on_ble_profile(1U, true);
    zassert_equal(codex_router_test_process_one(), -ENOMSG);
    zassert_true(router_fake_ble_purge_count() >= 2U);

    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    zassert_ok(codex_router_ingest_usb(payload));
    codex_router_on_usb_state(ZMK_USB_CONN_NONE);
    zassert_equal(codex_router_test_process_one(), -ENOMSG);
}

ZTEST(router, test_ble_change_does_not_purge_current_usb_ingress)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];

    codex_router_on_ble_profile(0U, true);
    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    make_fragment(payload, CODEX_CHANNEL_RPC,
                  "{\"m\":\"sys.version\",\"p\":null,\"id\":8}");
    zassert_ok(codex_router_ingest_usb(payload));
    codex_router_on_ble_profile(1U, true);
    process_ok();
    zassert_true(router_fake_usb_count() > 0U);
}

ZTEST(router, test_usb_edge_does_not_purge_current_ble_ingress)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];
    struct codex_ble_source_token token;

    codex_router_on_ble_profile(1U, true);
    make_fragment(payload, CODEX_CHANNEL_RPC,
                  "{\"m\":\"sys.version\",\"p\":null,\"id\":9}");
    token = (struct codex_ble_source_token){1U, router_fake_ble_generation()};
    zassert_ok(codex_router_ingest_ble(payload, &token));
    codex_router_on_usb_physical_state(USB_DC_RESET);
    process_ok();
    zassert_true(router_fake_ble_count() > 0U);
}

ZTEST(router, test_usb_physical_reenumeration_discards_old_partial)
{
    uint8_t first[CODEX_VENDOR_PAYLOAD_SIZE];
    uint8_t second[CODEX_VENDOR_PAYLOAD_SIZE];

    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    make_fragment(first, CODEX_CHANNEL_RPC, "{\"m\":\"sys.");
    zassert_ok(codex_router_ingest_usb(first));
    process_ok();
    codex_router_on_usb_physical_state(USB_DC_DISCONNECTED);
    codex_router_on_usb_physical_state(USB_DC_CONFIGURED);
    make_fragment(second, CODEX_CHANNEL_RPC,
                  "version\",\"p\":null,\"id\":1}");
    zassert_ok(codex_router_ingest_usb(second));
    zassert_true(codex_router_test_process_one() < 0);
    zassert_equal(router_fake_usb_count(), 0U);
}

ZTEST(router, test_same_profile_disconnect_reconnect_rejects_old_token_and_partial)
{
    uint8_t first[CODEX_VENDOR_PAYLOAD_SIZE];
    uint8_t second[CODEX_VENDOR_PAYLOAD_SIZE];
    struct codex_ble_source_token old;
    struct codex_ble_source_token fresh;

    codex_router_on_ble_profile(1U, true);
    old = (struct codex_ble_source_token){1U, router_fake_ble_generation()};
    make_fragment(first, CODEX_CHANNEL_RPC, "{\"m\":\"sys.");
    zassert_ok(codex_router_ingest_ble(first, &old));
    process_ok();
    codex_ble_hids_purge_queues();
    codex_router_on_ble_connection_edge(1U, false);
    codex_ble_hids_purge_queues();
    codex_router_on_ble_connection_edge(1U, true);
    zassert_equal(codex_router_ingest_ble(first, &old), -ESTALE);
    fresh = (struct codex_ble_source_token){1U, router_fake_ble_generation()};
    make_fragment(second, CODEX_CHANNEL_RPC,
                  "version\",\"p\":null,\"id\":1}");
    zassert_ok(codex_router_ingest_ble(second, &fresh));
    zassert_true(codex_router_test_process_one() < 0);
    zassert_equal(router_fake_ble_count(), 0U);
}

ZTEST(router, test_mid_response_ble_failure_aborts_connection_generation)
{
    static const uint8_t json[] =
        "{\"result\":{\"text\":\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\"},\"id\":10}";
    uint32_t generation;

    router_fake_set_status(1U, 0U, 80U);
    codex_router_on_ble_profile(1U, true);
    generation = router_fake_ble_generation();
    router_fake_fail_ble_after(2U, -EIO);
    zassert_equal(codex_router_send_json(CODEX_CHANNEL_RPC, json,
                                        sizeof(json) - 1U), -EIO);
    zassert_equal(router_fake_ble_count(), 2U);
    zassert_equal(router_fake_ble_abort_count(), 1U);
    zassert_true(router_fake_ble_generation() != generation);
    zassert_equal(codex_router_diagnostics_snapshot().aborted_responses, 1U);
    zassert_equal(router_fake_ble_references(0U), 0U);
}

ZTEST(router, test_same_profile_new_connection_never_receives_old_pinned_response)
{
    static const uint8_t json[] =
        "{\"result\":{\"text\":\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\"},\"id\":12}";

    router_fake_set_status(1U, 0U, 80U);
    codex_router_on_ble_profile(1U, true);
    router_fake_switch_ble_connection_on_notify(1U);
    zassert_equal(codex_router_send_json(CODEX_CHANNEL_RPC, json,
                                        sizeof(json) - 1U), -ESTALE);
    zassert_equal(router_fake_ble_notify_count(1U), 0U,
                  "a new same-profile connection must receive no old fragment");
    zassert_true(router_fake_ble_peak_references(0U) > 0U,
                 "the pinned old connection must be held by reference");
    zassert_equal(router_fake_ble_references(0U), 0U);
    zassert_equal(router_fake_ble_references(1U), 0U);
}

ZTEST(router, test_stale_old_response_abort_never_disconnects_new_connection)
{
    static const uint8_t json[] =
        "{\"result\":{\"text\":\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\"},\"id\":13}";

    router_fake_set_status(1U, 0U, 80U);
    codex_router_on_ble_profile(1U, true);
    router_fake_switch_ble_connection_on_notify(2U);
    router_fake_fail_ble_after(1U, -ESTALE);
    zassert_equal(codex_router_send_json(CODEX_CHANNEL_RPC, json,
                                        sizeof(json) - 1U), -ESTALE);
    zassert_equal(router_fake_ble_notify_count(0U), 1U);
    zassert_equal(router_fake_ble_notify_count(1U), 0U);
    zassert_equal(router_fake_ble_disconnect_count(1U), 0U,
                  "abort must never look up and disconnect the new active connection");
    zassert_equal(router_fake_ble_references(0U), 0U);
    zassert_equal(router_fake_ble_references(1U), 0U);
}

ZTEST(router, test_worker_reenumerates_after_partial_usb_response_before_next_json)
{
    static const uint8_t request[] =
        "{\"m\":\"sys.version\",\"p\":null,\"id\":\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\"}";
    uint8_t next[CODEX_VENDOR_PAYLOAD_SIZE];

    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    router_fake_fail_usb_after(1U, -EIO);
    zassert_ok(codex_framing_encode(CODEX_CHANNEL_RPC, request,
                                    sizeof(request) - 1U,
                                    enqueue_usb_fragment, NULL));
    codex_router_test_submit_work();
    zassert_ok(router_fake_wait_usb_attempts(2U, K_MSEC(100)));
    k_sleep(K_MSEC(20));
    zassert_equal(router_fake_usb_disable_count(), 1U,
                  "partial response requires a host-visible USB detach boundary");
    zassert_false(usb_session_has_crlf(0U),
                  "the pre-reset session contains only an incomplete prefix");

    router_fake_clear_usb_failure();
    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    make_fragment(next, CODEX_CHANNEL_RPC,
                  "{\"m\":\"sys.version\",\"p\":null,\"id\":14}");
    zassert_ok(codex_router_ingest_usb(next));
    codex_router_test_submit_work();
    zassert_ok(router_fake_wait_usb_attempts(3U, K_MSEC(100)));
    k_sleep(K_MSEC(5));
    zassert_true(usb_session_has_crlf(1U),
                 "only a complete response may appear after re-enumeration");
}

ZTEST(router, test_usb_recovery_rejects_ingress_across_physical_edges_and_retries_enable)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];

    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    router_fake_fail_usb_after(1U, -EIO);
    router_fake_fail_usb_enable(1U, -EIO);
    zassert_equal(send_multifragment_usb_response(), -EIO);
    zassert_ok(router_fake_wait_usb_disables(1U, K_MSEC(100)));
    make_fragment(payload, CODEX_CHANNEL_DEBUG, "{}");
    codex_router_on_usb_physical_state(USB_DC_RESET);
    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    zassert_equal(codex_router_ingest_usb(payload), -EAGAIN,
                  "USB ingress must fail explicitly until recovery completes");
    zassert_ok(router_fake_wait_usb_enables(1U, K_MSEC(100)));
    zassert_equal(codex_router_ingest_usb(payload), -EAGAIN,
                  "failed re-enable must keep the recovery gate closed");
    zassert_ok(router_fake_wait_usb_enables(2U, K_MSEC(250)));
    zassert_equal(codex_router_diagnostics_snapshot().usb_recovery_errors, 1U);
}

ZTEST(router, test_two_consecutive_partial_usb_responses_each_reenumerate)
{
    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    router_fake_fail_usb_after(1U, -EIO);
    zassert_equal(send_multifragment_usb_response(), -EIO);
    zassert_ok(router_fake_wait_usb_enables(1U, K_MSEC(100)));

    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    zassert_equal(send_multifragment_usb_response(), -EIO);
    zassert_ok(router_fake_wait_usb_enables(2U, K_MSEC(100)));
    zassert_equal(router_fake_usb_disable_count(), 2U);
    zassert_equal(codex_router_diagnostics_snapshot().usb_recoveries, 2U);
}

ZTEST(router, test_old_ble_token_is_rejected)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];
    struct codex_ble_source_token old;

    make_fragment(payload, CODEX_CHANNEL_DEBUG, "{}");
    codex_router_on_ble_profile(0U, true);
    old = (struct codex_ble_source_token){0U, router_fake_ble_generation()};
    codex_router_on_ble_profile(1U, true);
    zassert_equal(codex_router_ingest_ble(payload, &old), -ESTALE);
}

ZTEST(router, test_rpc_emit_error_propagates_from_worker)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];

    make_fragment(payload, CODEX_CHANNEL_RPC,
                  "{\"m\":\"sys.version\",\"p\":null,\"id\":1}");
    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    router_fake_set_send_error(-EIO);
    zassert_ok(codex_router_ingest_usb(payload));
    zassert_equal(codex_router_test_process_one(), -EIO);
    zassert_equal(codex_router_diagnostics_snapshot().emit_errors, 1U);
}

ZTEST(router, test_no_connection_returns_enotconn)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE] = {0};
    static const uint8_t json[] = "{}";

    zassert_equal(codex_router_send_vendor(payload), -ENOTCONN);
    zassert_equal(codex_router_send_json(CODEX_CHANNEL_RPC, json,
                                        sizeof(json) - 1U), -ENOTCONN);
}

ZTEST(router, test_powered_only_reports_charging_without_stealing_ble)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];
    struct codex_ble_source_token token;
    char response[CODEX_JSON_MAX_SIZE + 3U];

    router_fake_set_usb_powered(true);
    router_fake_set_status(2U, 5U, 77U);
    codex_router_on_ble_profile(2U, true);
    codex_router_on_usb_state(ZMK_USB_CONN_POWERED);
    make_fragment(payload, CODEX_CHANNEL_RPC,
                  "{\"m\":\"device.status\",\"p\":null,\"id\":3}");
    token = (struct codex_ble_source_token){2U, router_fake_ble_generation()};
    zassert_ok(codex_router_ingest_ble(payload, &token));
    process_ok();
    collect_response(CODEX_TRANSPORT_BLE, response, sizeof(response));
    zassert_not_null(strstr(response, "\"is_charging\":true"));
}

ZTEST(router, test_generation_wrap_is_monotonic_modulo_uint32)
{
    codex_router_test_set_generations(UINT32_MAX, UINT32_MAX);
    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    zassert_equal(codex_router_state().usb_generation, 0U);
    codex_router_on_ble_profile(1U, true);
    zassert_equal(codex_router_state().ble_generation, 0U);
}

ZTEST(router, test_multifragment_response_is_pinned_to_one_transport)
{
    static const uint8_t json[] =
        "{\"result\":{\"text\":\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\"},\"id\":9}";

    codex_router_on_ble_profile(0U, true);
    zassert_ok(codex_router_send_json(CODEX_CHANNEL_RPC, json, sizeof(json) - 1U));
    zassert_true(router_fake_ble_count() > 1U);
    zassert_equal(router_fake_usb_count(), 0U);
    for (size_t i = 0U; i < router_fake_ble_count(); i++) {
        zassert_equal(router_fake_ble_packet(i)[0], CODEX_CHANNEL_RPC);
    }
}

#define ROUTE_THREAD_STACK_SIZE 1024
K_THREAD_STACK_DEFINE(send_stack, ROUTE_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(switch_stack, ROUTE_THREAD_STACK_SIZE);
static struct k_thread send_thread;
static struct k_thread switch_thread;
static K_SEM_DEFINE(send_done, 0, 1);
static K_SEM_DEFINE(switch_done, 0, 1);
static int concurrent_send_result;

static void concurrent_send(void *json_pointer, void *len_pointer, void *unused)
{
    ARG_UNUSED(unused);
    concurrent_send_result = codex_router_send_json(
        CODEX_CHANNEL_RPC, json_pointer, (size_t)(uintptr_t)len_pointer);
    k_sem_give(&send_done);
}

static void concurrent_switch(void *unused1, void *unused2, void *unused3)
{
    ARG_UNUSED(unused1);
    ARG_UNUSED(unused2);
    ARG_UNUSED(unused3);
    codex_router_on_usb_state(ZMK_USB_CONN_NONE);
    k_sem_give(&switch_done);
}

ZTEST(router, test_route_switch_waits_for_all_fragments_of_pinned_response)
{
    static const uint8_t json[] =
        "{\"result\":{\"text\":\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\"},\"id\":10}";
    uint8_t ingress[CODEX_VENDOR_PAYLOAD_SIZE];

    codex_router_on_ble_profile(0U, true);
    codex_router_on_usb_state(ZMK_USB_CONN_HID);
    router_fake_block_first_usb_send(true);
    k_sem_reset(&send_done);
    k_sem_reset(&switch_done);
    k_thread_create(&send_thread, send_stack, K_THREAD_STACK_SIZEOF(send_stack),
                    concurrent_send, (void *)json,
                    (void *)(uintptr_t)(sizeof(json) - 1U), NULL,
                    K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
    zassert_ok(router_fake_wait_usb_send_entered(K_MSEC(100)));
    make_fragment(ingress, CODEX_CHANNEL_DEBUG, "{}");
    zassert_equal(codex_router_ingest_usb(ingress), -EAGAIN,
                  "USB callback must never block behind response emission");
    k_thread_create(&switch_thread, switch_stack,
                    K_THREAD_STACK_SIZEOF(switch_stack), concurrent_switch,
                    NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
    zassert_equal(k_sem_take(&switch_done, K_MSEC(5)), -EAGAIN,
                  "route switch must wait on the pinned response mutex");
    router_fake_release_usb_send();
    zassert_ok(k_sem_take(&send_done, K_MSEC(100)));
    zassert_ok(k_sem_take(&switch_done, K_MSEC(100)));
    zassert_ok(k_thread_join(&send_thread, K_MSEC(100)));
    zassert_ok(k_thread_join(&switch_thread, K_MSEC(100)));
    zassert_ok(concurrent_send_result);
    zassert_true(router_fake_usb_count() > 1U);
    zassert_equal(router_fake_ble_count(), 0U);
    zassert_equal(codex_router_diagnostics_snapshot().ingress_busy, 1U);
    zassert_equal(codex_router_state().active, CODEX_TRANSPORT_BLE);
}

ZTEST_SUITE(router, NULL, NULL, router_before, NULL, NULL);
