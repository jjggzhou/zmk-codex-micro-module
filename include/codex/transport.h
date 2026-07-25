#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/usb.h>

#include <codex/ble_hids.h>
#include <codex/framing.h>

#define CODEX_ROUTER_INGRESS_DEPTH 4U

struct codex_route_state {
    enum codex_transport active;
    bool has_active;
    bool usb_hid_ready;
    bool ble_connected;
    uint8_t ble_profile;
    uint32_t usb_generation;
    uint32_t ble_generation;
};

struct codex_router_diagnostics {
    uint32_t ingress_full;
    uint32_t ingress_busy;
    uint32_t stale_input;
    uint32_t debug_messages;
    uint32_t framing_errors;
    uint32_t rpc_errors;
    uint32_t emit_errors;
    uint32_t aborted_responses;
};

void codex_router_on_usb_state(enum zmk_usb_conn_state state);
void codex_router_on_usb_physical_state(enum usb_dc_status_code status);
void codex_router_on_ble_profile(uint8_t profile, bool connected);
void codex_router_on_ble_connection_edge(uint8_t profile, bool connected);
struct codex_route_state codex_router_state(void);

int codex_router_ingest_usb(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE]);
int codex_router_ingest_ble(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
    const struct codex_ble_source_token *token);
int codex_router_send_vendor(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE]);
int codex_router_send_json(enum codex_channel channel, const uint8_t *json,
                           size_t len);
struct codex_router_diagnostics codex_router_diagnostics_snapshot(void);

#if defined(CONFIG_ZTEST)
void codex_router_test_reset(void);
int codex_router_test_process_one(void);
void codex_router_test_set_generations(uint32_t usb_generation,
                                       uint32_t ble_generation);
#endif
