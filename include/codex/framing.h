#pragma once

#include <stddef.h>
#include <stdint.h>

#include <codex/descriptor.h>

#define CODEX_FRAGMENT_MAX (CODEX_VENDOR_PAYLOAD_SIZE - 2U)
#define CODEX_JSON_MAX_SIZE 1024U
#define CODEX_JSON_MAX_DEPTH 32U

enum codex_transport {
    CODEX_TRANSPORT_USB = 0,
    CODEX_TRANSPORT_BLE = 1,
    CODEX_TRANSPORT_COUNT,
};

enum codex_channel {
    CODEX_CHANNEL_DEBUG = 0x01,
    CODEX_CHANNEL_RPC = 0x02,
};

typedef void (*codex_json_cb_t)(enum codex_transport transport,
                                enum codex_channel channel,
                                const uint8_t *json, size_t len);

typedef int (*codex_emit_report_t)(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE], void *ctx);

int codex_framing_ingest(enum codex_transport transport,
                         const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
                         uint32_t connection_generation,
                         codex_json_cb_t on_json);

int codex_framing_encode(enum codex_channel channel,
                         const uint8_t *json, size_t len,
                         codex_emit_report_t emit, void *ctx);

void codex_framing_reset_transport(enum codex_transport transport);
