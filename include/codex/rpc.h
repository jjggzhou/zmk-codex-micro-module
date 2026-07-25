#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <codex/framing.h>

#define CODEX_COMPAT_FIRMWARE_VERSION "0.4.1"
#if defined(CONFIG_CODEX_FIRMWARE_VERSION)
#define CODEX_FIRMWARE_VERSION CONFIG_CODEX_FIRMWARE_VERSION
#else
#define CODEX_FIRMWARE_VERSION CODEX_COMPAT_FIRMWARE_VERSION
#endif
#define CODEX_RPC_ERROR_FORBIDDEN 403
#define CODEX_RPC_ERROR_METHOD_NOT_FOUND 404

struct codex_device_status {
    const char *version;
    uint8_t profile_index;
    uint8_t layer_index;
    uint8_t battery_percent;
    bool is_charging;
};

/* The callback consumes one complete JSON value on the request's transport. */
typedef int (*codex_rpc_emit_t)(enum codex_transport source,
                                const uint8_t *json, size_t len, void *ctx);

int codex_rpc_dispatch(enum codex_transport source, const uint8_t *json,
                       size_t len, codex_rpc_emit_t emit, void *ctx);

struct codex_device_status codex_device_status_snapshot(void);
