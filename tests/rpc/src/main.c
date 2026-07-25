#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <codex/rpc.h>

#include "json_value.h"
#include "zmk_fakes.h"

struct capture {
    enum codex_transport source;
    uint8_t json[CODEX_JSON_MAX_SIZE];
    size_t len;
    size_t count;
    int result;
};

static int capture_emit(enum codex_transport source, const uint8_t *json,
                        size_t len, void *ctx)
{
    struct capture *capture = ctx;

    zassert_not_null(json);
    zassert_true(len <= sizeof(capture->json));
    zassert_equal(codex_json_value_validate(json, len, CODEX_JSON_MAX_DEPTH),
                  CODEX_JSON_COMPLETE);
    capture->source = source;
    capture->len = len;
    capture->count++;
    memcpy(capture->json, json, len);
    return capture->result;
}

static int dispatch_bytes(const uint8_t *json, size_t len, struct capture *capture)
{
    memset(capture, 0, sizeof(*capture));
    return codex_rpc_dispatch(CODEX_TRANSPORT_USB, json, len, capture_emit, capture);
}

static int dispatch(const char *json, struct capture *capture)
{
    return dispatch_bytes((const uint8_t *)json, strlen(json), capture);
}

static bool response_has(const struct capture *capture, const char *needle)
{
    size_t needle_len = strlen(needle);

    if (needle_len > capture->len) {
        return false;
    }
    for (size_t i = 0U; i <= capture->len - needle_len; i++) {
        if (memcmp(&capture->json[i], needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static void expect_success(const char *request, const char *method,
                           const char *result_fragment)
{
    struct capture capture;

    zassert_ok(dispatch(request, &capture));
    zassert_equal(capture.count, 1U);
    zassert_equal(capture.source, CODEX_TRANSPORT_USB);
    zassert_true(response_has(&capture, method));
    zassert_true(response_has(&capture, result_fragment));
}

ZTEST(rpc, test_dispatches_both_request_shapes_and_all_supported_methods)
{
    static const struct {
        const char *method;
        const char *result;
    } cases[] = {
        {"sys.version", "\"version\":\"0.4.1\""},
        {"device.status", "\"battery\":50"},
        {"v.oai.rgbcfg", "\"ok\":1"},
        {"v.oai.thstatus", "\"ok\":1"},
        {"lights.preview", "\"result\":null"},
        {"ui.active_screen", "\"result\":null"},
        {"ui.home_accent_color", "\"result\":null"},
        {"host.focused_app", "\"ok\":1"},
    };
    char request[180];

    rpc_fake_status_set(1, 2, 50, true);
    for (size_t i = 0U; i < ARRAY_SIZE(cases); i++) {
        snprintk(request, sizeof(request),
                 "{\"m\":\"%s\",\"p\":null,\"id\":%u}", cases[i].method,
                 (unsigned int)i + 1U);
        expect_success(request, cases[i].method, cases[i].result);
        snprintk(request, sizeof(request),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"%s\","
                 "\"params\":{},\"id\":\"s%u\"}", cases[i].method,
                 (unsigned int)i);
        expect_success(request, cases[i].method, cases[i].result);
    }
}

ZTEST(rpc, test_device_status_is_a_fresh_bounded_zmk_snapshot)
{
    struct capture capture;
    struct codex_device_status status;
    const char *request = "{\"m\":\"device.status\",\"id\":1}";

    rpc_fake_status_set(2, 5, 0, false);
    status = codex_device_status_snapshot();
    zassert_equal(strcmp(status.version, "0.4.1"), 0);
    zassert_equal(status.profile_index, 2U);
    zassert_equal(status.layer_index, 5U);
    zassert_equal(status.battery_percent, 0U);
    zassert_false(status.is_charging);
    zassert_ok(dispatch(request, &capture));
    zassert_true(response_has(&capture, "\"profile_index\":2"));
    zassert_true(response_has(&capture, "\"layer_index\":5"));
    zassert_true(response_has(&capture, "\"battery\":0"));
    zassert_true(response_has(&capture, "\"is_charging\":false"));

    rpc_fake_status_set(-1, 1, 101, true);
    status = codex_device_status_snapshot();
    zassert_equal(status.profile_index, 0U);
    zassert_equal(status.battery_percent, 100U);
    zassert_true(status.is_charging);
    zassert_ok(dispatch(request, &capture));
    zassert_true(response_has(&capture, "\"profile_index\":0"));
    zassert_true(response_has(&capture, "\"layer_index\":1"));
    zassert_true(response_has(&capture, "\"battery\":100"));
    zassert_true(response_has(&capture, "\"is_charging\":true"));

    rpc_fake_status_set(256, 0, 100, false);
    status = codex_device_status_snapshot();
    zassert_equal(status.profile_index, 0U);
}

ZTEST(rpc, test_unknown_and_dangerous_methods_have_stable_errors)
{
    static const char *const dangerous[] = {
        "sys.bootloader", "sys.selftest", "fs.list", "mp.write_info",
        "wlsdk.widget",
    };
    struct capture capture;
    char request[120];

    zassert_ok(dispatch("{\"m\":\"not.real\",\"id\":3}", &capture));
    zassert_true(response_has(&capture, "\"code\":404"));
    for (size_t i = 0U; i < ARRAY_SIZE(dangerous); i++) {
        snprintk(request, sizeof(request), "{\"m\":\"%s\",\"id\":4}",
                 dangerous[i]);
        zassert_ok(dispatch(request, &capture));
        zassert_true(response_has(&capture, "\"code\":403"));
    }

    zassert_ok(dispatch("{\"m\":\"filesystem.list\",\"id\":5}", &capture));
    zassert_true(response_has(&capture, "\"code\":404"));
    zassert_ok(dispatch("{\"m\":\"fsx.list\",\"id\":6}", &capture));
    zassert_true(response_has(&capture, "\"code\":404"));
}

ZTEST(rpc, test_notifications_do_not_emit)
{
    struct capture capture;

    zassert_ok(dispatch("{\"m\":\"sys.version\",\"p\":null}", &capture));
    zassert_equal(capture.count, 0U);
}

ZTEST(rpc, test_ids_are_type_checked_and_safely_echoed)
{
    struct capture capture;

    zassert_ok(dispatch("{\"m\":\"sys.version\",\"id\":-12}", &capture));
    zassert_true(response_has(&capture, "\"id\":-12"));
    zassert_ok(dispatch("{\"m\":\"sys.version\",\"id\":\"x\\\",\\\"owned\\\":true\"}",
                        &capture));
    zassert_true(response_has(&capture,
                              "\"id\":\"x\\\",\\\"owned\\\":true\""));
    zassert_false(response_has(&capture, "\"owned\":true,"));
    zassert_ok(dispatch("{\"m\":\"sys.version\",\"id\":null}", &capture));
    zassert_true(response_has(&capture, "\"id\":null"));

    zassert_equal(dispatch("{\"m\":\"sys.version\",\"id\":true}", &capture),
                  -EINVAL);
    zassert_equal(capture.count, 0U);
}

ZTEST(rpc, test_token_aware_parser_rejects_ambiguous_or_malformed_requests)
{
    static const char *const invalid[] = {
        "{\"m\":\"sys.version\",\"m\":\"device.status\",\"id\":1}",
        "{\"m\":\"sys.version\",\"\\u006d\":\"device.status\",\"id\":1}",
        "{\"method\":\"sys.version\",\"m\":\"sys.version\",\"id\":1}",
        "{\"m\":7,\"id\":1}",
        "{\"m\":\"sys.version\",\"p\":7,\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"sys.version\","
        "\"params\":\"wrong\",\"id\":1}",
        "{\"nested\":{\"method\":\"sys.version\"},\"id\":1}",
        "{\"jsonrpc\":2.0,\"method\":\"sys.version\",\"id\":1}",
        "{\"jsonrpc\":\"1.0\",\"method\":\"sys.version\",\"id\":1}",
        "{\"m\":\"sys.version\",\"id\":1,\"id\":2}",
        "{\"m\":\"sys.version\",\"id\":1} trailing",
        "{\"m\":\"sys.version\",\"id\":1",
    };
    struct capture capture;

    for (size_t i = 0U; i < ARRAY_SIZE(invalid); i++) {
        zassert_equal(dispatch(invalid[i], &capture), -EINVAL, "%s", invalid[i]);
        zassert_equal(capture.count, 0U);
    }

    zassert_ok(dispatch("{\"m\":\"sys\\u002eversion\",\"id\":9}", &capture));
    zassert_true(response_has(&capture, "\"version\":\"0.4.1\""));
    zassert_ok(dispatch("{\"m\":\"sys.version\",\"p\":{\"method\":"
                        "\"device.status\",\"nested\":[1,{\"id\":99}]},\"id\":10}",
                        &capture));
    zassert_true(response_has(&capture, "\"id\":10"));
    zassert_false(response_has(&capture, "\"battery\":"));

    zassert_ok(dispatch("{\"m\":\"fs\\u002elist\",\"id\":11}", &capture));
    zassert_true(response_has(&capture, "\"code\":403"));
}

ZTEST(rpc, test_emit_failure_is_returned_unchanged)
{
    struct capture capture = {.result = -EIO};
    const char *request = "{\"m\":\"sys.version\",\"id\":1}";

    zassert_equal(codex_rpc_dispatch(CODEX_TRANSPORT_BLE,
                                     (const uint8_t *)request, strlen(request),
                                     capture_emit, &capture), -EIO);
    zassert_equal(capture.count, 1U);
    zassert_equal(capture.source, CODEX_TRANSPORT_BLE);
}

ZTEST(rpc, test_rejects_response_that_would_exceed_fixed_buffer)
{
    uint8_t request[CODEX_JSON_MAX_SIZE];
    struct capture capture;
    static const char prefix[] = "{\"m\":\"sys.version\",\"id\":\"";
    size_t suffix_at = CODEX_JSON_MAX_SIZE - 3U;

    memcpy(request, prefix, sizeof(prefix) - 1U);
    memset(&request[sizeof(prefix) - 1U], 'a', suffix_at - (sizeof(prefix) - 1U));
    request[suffix_at] = '"';
    request[suffix_at + 1U] = '}';
    zassert_equal(dispatch_bytes(request, suffix_at + 2U, &capture), -EMSGSIZE);
    zassert_equal(capture.count, 0U);
}

ZTEST(rpc, test_argument_validation)
{
    struct capture capture;
    static const uint8_t request[] = "{\"m\":\"sys.version\",\"id\":1}";

    zassert_equal(codex_rpc_dispatch(CODEX_TRANSPORT_COUNT, request,
                                    sizeof(request) - 1U, capture_emit, &capture),
                  -EINVAL);
    zassert_equal(codex_rpc_dispatch(CODEX_TRANSPORT_USB, NULL, 1U,
                                    capture_emit, &capture), -EINVAL);
    zassert_equal(codex_rpc_dispatch(CODEX_TRANSPORT_USB, request,
                                    sizeof(request) - 1U, NULL, &capture), -EINVAL);
    zassert_equal(codex_rpc_dispatch(CODEX_TRANSPORT_USB, request, 0U,
                                    capture_emit, &capture), -EINVAL);
    zassert_equal(codex_rpc_dispatch(CODEX_TRANSPORT_USB, request,
                                    CODEX_JSON_MAX_SIZE + 1U, capture_emit, &capture),
                  -EMSGSIZE);
}

ZTEST_SUITE(rpc, NULL, NULL, NULL, NULL, NULL);
