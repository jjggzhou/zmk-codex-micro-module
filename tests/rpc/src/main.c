#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <codex/rpc.h>
#include <codex/lighting.h>
#include <codex/state.h>

#include "json_value.h"
#include "state_internal.h"
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

struct ordering_capture {
    struct capture response;
    enum codex_effect observed_effect;
};

struct concurrent_emit_context {
    struct k_sem outer_entered;
    struct k_sem inner_emitted;
    struct capture inner;
    uint8_t outer_response[CODEX_JSON_MAX_SIZE];
    size_t outer_len;
    int inner_result;
};

K_THREAD_STACK_DEFINE(concurrent_emit_stack, 2048);
static struct k_thread concurrent_emit_thread;

static int inner_emit(enum codex_transport source, const uint8_t *json,
                      size_t len, void *ctx)
{
    struct concurrent_emit_context *context = ctx;
    int err = capture_emit(source, json, len, &context->inner);

    k_sem_give(&context->inner_emitted);
    return err;
}

static int waiting_outer_emit(enum codex_transport source, const uint8_t *json,
                              size_t len, void *ctx)
{
    struct concurrent_emit_context *context = ctx;

    ARG_UNUSED(source);
    zassert_true(len <= sizeof(context->outer_response));
    context->outer_len = len;
    memcpy(context->outer_response, json, len);
    k_sem_give(&context->outer_entered);
    if (k_sem_take(&context->inner_emitted, K_MSEC(100)) != 0) {
        return -ETIMEDOUT;
    }
    return memcmp(context->outer_response, json, len) == 0 ? 0 : -EILSEQ;
}

static void concurrent_dispatch(void *arg1, void *arg2, void *arg3)
{
    struct concurrent_emit_context *context = arg1;
    static const char request[] = "{\"m\":\"sys.version\",\"id\":2}";

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    k_sem_take(&context->outer_entered, K_FOREVER);
    context->inner_result = codex_rpc_dispatch(
        CODEX_TRANSPORT_BLE, (const uint8_t *)request, strlen(request),
        inner_emit, context);
}

static int ordering_emit(enum codex_transport source, const uint8_t *json,
                         size_t len, void *ctx)
{
    struct ordering_capture *capture = ctx;

    capture->observed_effect = codex_lighting_snapshot().keys.effect;
    return capture_emit(source, json, len, &capture->response);
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
                           const char *result_fragment, bool standard)
{
    struct capture capture;

    zassert_ok(dispatch(request, &capture));
    zassert_equal(capture.count, 1U);
    zassert_equal(capture.source, CODEX_TRANSPORT_USB);
    if (!standard) {
        zassert_true(response_has(&capture, method));
    }
    zassert_true(response_has(&capture, result_fragment));
    zassert_equal(response_has(&capture, "\"jsonrpc\":\"2.0\""), standard);
    zassert_equal(response_has(&capture, "\"method\":"), !standard);
}

static void before(void *fixture)
{
    ARG_UNUSED(fixture);
    codex_lighting_test_reset();
    codex_indicators_reset();
}

ZTEST(rpc, test_dispatches_both_request_shapes_and_all_supported_methods)
{
    static const struct {
        const char *method;
        const char *params;
        const char *result;
    } cases[] = {
        {"sys.version", "null", "\"version\":\"0.4.1\""},
        {"device.status", "{}", "\"battery\":50"},
        {"v.oai.rgbcfg", "{\"keys\":{\"e\":1}}", "\"ok\":1"},
        {"v.oai.thstatus", "[{\"id\":0,\"e\":1}]", "\"ok\":1"},
        {"lights.preview", "null", "\"result\":null"},
        {"ui.active_screen", "{}", "\"result\":null"},
        {"ui.home_accent_color", "{}", "\"result\":null"},
        {"host.focused_app", "{}", "\"ok\":1"},
    };
    char request[180];

    rpc_fake_status_set(1, 2, 50, true);
    for (size_t i = 0U; i < ARRAY_SIZE(cases); i++) {
        snprintk(request, sizeof(request),
                 "{\"m\":\"%s\",\"p\":%s,\"id\":%u}", cases[i].method,
                 cases[i].params, (unsigned int)i + 1U);
        expect_success(request, cases[i].method, cases[i].result, false);
        snprintk(request, sizeof(request),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"%s\","
                 "\"params\":%s,\"id\":\"s%u\"}", cases[i].method,
                 cases[i].params, (unsigned int)i);
        expect_success(request, cases[i].method, cases[i].result, true);
    }
}

ZTEST(rpc, test_lighting_methods_apply_compact_and_standard_params_before_ack)
{
    struct capture capture;
    struct codex_lighting_model model;

    zassert_ok(dispatch("{\"m\":\"v.oai.rgbcfg\",\"p\":{\"keys\":{\"e\":1,"
                        "\"c\":66051}},\"id\":7}", &capture));
    static const char compact_ack[] =
        "{\"result\":{\"ok\":1},\"id\":7,\"method\":\"v.oai.rgbcfg\"}";

    zassert_equal(capture.len, sizeof(compact_ack) - 1U);
    zassert_mem_equal(capture.json, compact_ack, sizeof(compact_ack) - 1U);
    model = codex_lighting_snapshot();
    zassert_equal(model.keys.effect, CODEX_EFFECT_SOLID);
    zassert_equal(model.keys.color, 0x010203U);

    zassert_ok(dispatch("{\"jsonrpc\":\"2.0\",\"method\":\"v.oai.thstatus\","
                        "\"params\":[{\"id\":4,\"e\":3,\"sk\":1}],\"id\":8}",
                        &capture));
    static const char standard_ack[] =
        "{\"jsonrpc\":\"2.0\",\"result\":{\"ok\":1},\"id\":8}";

    zassert_equal(capture.len, sizeof(standard_ack) - 1U);
    zassert_mem_equal(capture.json, standard_ack, sizeof(standard_ack) - 1U);
    model = codex_lighting_snapshot();
    zassert_equal(model.agents[4].zone.effect, CODEX_EFFECT_RAINBOW);
    zassert_true(model.agents[4].sk);
}

ZTEST(rpc, test_lighting_notifications_commit_without_emitting)
{
    struct capture capture;

    zassert_ok(dispatch("{\"m\":\"v.oai.rgbcfg\",\"p\":{\"ambient\":{\"e\":5}}}",
                        &capture));
    zassert_equal(capture.count, 0U);
    zassert_equal(codex_lighting_snapshot().ambient.effect, CODEX_EFFECT_GRADIENT);

    zassert_ok(dispatch("{\"jsonrpc\":\"2.0\",\"method\":\"v.oai.thstatus\","
                        "\"params\":[{\"id\":2,\"e\":6}]}", &capture));
    zassert_equal(capture.count, 0U);
    zassert_equal(codex_lighting_snapshot().agents[2].zone.effect,
                  CODEX_EFFECT_SHALLOW_BREATH);
}

ZTEST(rpc, test_invalid_lighting_params_have_stable_errors_and_atomic_state)
{
    struct capture capture;

    zassert_ok(dispatch("{\"m\":\"v.oai.rgbcfg\",\"p\":{\"keys\":{\"e\":1}},"
                        "\"id\":1}", &capture));
    struct codex_lighting_model before = codex_lighting_snapshot();
    static const struct {
        const char *request;
        const char *response;
    } invalid[] = {
        {"{\"m\":\"v.oai.rgbcfg\",\"id\":7}",
         "{\"error\":{\"code\":400,\"message\":\"Invalid params\"},\"id\":7}"},
        {"{\"m\":\"v.oai.rgbcfg\",\"p\":null,\"id\":7}",
         "{\"error\":{\"code\":400,\"message\":\"Invalid params\"},\"id\":7}"},
        {"{\"m\":\"v.oai.rgbcfg\",\"p\":[],\"id\":7}",
         "{\"error\":{\"code\":400,\"message\":\"Invalid params\"},\"id\":7}"},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"v.oai.thstatus\","
         "\"params\":{},\"id\":9}",
         "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":400,"
         "\"message\":\"Invalid params\"},\"id\":9}"},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"v.oai.thstatus\","
         "\"params\":[{\"id\":1,\"e\":2},{\"id\":1,\"e\":3}],\"id\":9}",
         "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":400,"
         "\"message\":\"Invalid params\"},\"id\":9}"},
        {"{\"m\":\"v.oai.rgbcfg\",\"p\":{\"keys\":{\"e\":2},"
         "\"ambient\":{\"e\":3,\"e\":4}},\"id\":7}",
         "{\"error\":{\"code\":400,\"message\":\"Invalid params\"},\"id\":7}"},
    };

    for (size_t i = 0U; i < ARRAY_SIZE(invalid); i++) {
        struct codex_lighting_model after;

        zassert_ok(dispatch(invalid[i].request, &capture), "%s", invalid[i].request);
        zassert_equal(capture.len, strlen(invalid[i].response));
        zassert_mem_equal(capture.json, invalid[i].response, capture.len);
        after = codex_lighting_snapshot();
        zassert_mem_equal(&after, &before, sizeof(before));
    }
}

ZTEST(rpc, test_invalid_lighting_notification_returns_error_without_response_or_mutation)
{
    struct capture capture;
    struct codex_lighting_model before = codex_lighting_snapshot();

    zassert_equal(dispatch("{\"m\":\"v.oai.rgbcfg\",\"p\":{}}", &capture),
                  -EINVAL);
    zassert_equal(capture.count, 0U);
    struct codex_lighting_model after = codex_lighting_snapshot();

    zassert_mem_equal(&after, &before, sizeof(before));
}

ZTEST(rpc, test_preview_and_lighting_updates_never_touch_mono_indicator_state)
{
    struct capture capture;

    codex_indicators_render(CODEX_INDICATOR_MIDDLE | CODEX_INDICATOR_BOTTOM);
    zassert_equal(codex_indicator_bits(),
                  CODEX_INDICATOR_MIDDLE | CODEX_INDICATOR_BOTTOM);
    zassert_ok(dispatch("{\"m\":\"v.oai.rgbcfg\",\"p\":{\"keys\":{\"e\":1}},"
                        "\"id\":1}", &capture));
    struct codex_lighting_model before = codex_lighting_snapshot();

    zassert_ok(dispatch("{\"jsonrpc\":\"2.0\",\"method\":\"lights.preview\","
                        "\"params\":{\"e\":6,\"c\":16777215},\"id\":2}",
                        &capture));
    struct codex_lighting_model after = codex_lighting_snapshot();

    zassert_mem_equal(&after, &before, sizeof(before));
    zassert_equal(codex_indicator_bits(),
                  CODEX_INDICATOR_MIDDLE | CODEX_INDICATOR_BOTTOM);
}

ZTEST(rpc, test_emit_failure_does_not_roll_back_lighting_commit)
{
    struct capture capture = {.result = -EIO};
    const char *request = "{\"m\":\"v.oai.rgbcfg\",\"p\":{\"keys\":{\"e\":4}},"
                          "\"id\":1}";

    zassert_equal(codex_rpc_dispatch(CODEX_TRANSPORT_BLE,
                                     (const uint8_t *)request, strlen(request),
                                     capture_emit, &capture), -EIO);
    zassert_equal(capture.count, 1U);
    zassert_equal(codex_lighting_snapshot().keys.effect, CODEX_EFFECT_BREATH);
}

ZTEST(rpc, test_lighting_commit_is_visible_inside_ack_emit_callback)
{
    struct ordering_capture capture = {0};
    const char *request = "{\"m\":\"v.oai.rgbcfg\",\"p\":{\"keys\":{\"e\":5}},"
                          "\"id\":1}";

    zassert_ok(codex_rpc_dispatch(CODEX_TRANSPORT_USB,
                                  (const uint8_t *)request, strlen(request),
                                  ordering_emit, &capture));
    zassert_equal(capture.response.count, 1U);
    zassert_equal(capture.observed_effect, CODEX_EFFECT_GRADIENT);
}

ZTEST(rpc, test_concurrent_emit_has_stable_bytes_without_internal_lock)
{
    struct concurrent_emit_context context = {0};
    static const char request[] = "{\"m\":\"sys.version\",\"id\":1}";

    k_sem_init(&context.outer_entered, 0, 1);
    k_sem_init(&context.inner_emitted, 0, 1);
    k_thread_create(&concurrent_emit_thread, concurrent_emit_stack,
                    K_THREAD_STACK_SIZEOF(concurrent_emit_stack),
                    concurrent_dispatch, &context, NULL, NULL,
                    K_PRIO_PREEMPT(1), 0, K_NO_WAIT);

    int outer_result = codex_rpc_dispatch(
        CODEX_TRANSPORT_USB, (const uint8_t *)request, strlen(request),
        waiting_outer_emit, &context);

    zassert_ok(k_thread_join(&concurrent_emit_thread, K_SECONDS(1)));
    zassert_ok(outer_result);
    zassert_ok(context.inner_result);
    zassert_equal(context.inner.count, 1U);
    zassert_true(response_has(&context.inner, "\"id\":2"));
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
    zassert_false(response_has(&capture, "\"jsonrpc\":"));
    zassert_ok(dispatch("{\"jsonrpc\":\"2.0\",\"method\":\"not.real\","
                        "\"id\":3}", &capture));
    zassert_true(response_has(&capture, "\"code\":404"));
    zassert_true(response_has(&capture, "\"jsonrpc\":\"2.0\""));
    zassert_false(response_has(&capture, "\"method\":"));
    for (size_t i = 0U; i < ARRAY_SIZE(dangerous); i++) {
        snprintk(request, sizeof(request), "{\"m\":\"%s\",\"id\":4}",
                 dangerous[i]);
        zassert_ok(dispatch(request, &capture));
        zassert_true(response_has(&capture, "\"code\":403"));
        snprintk(request, sizeof(request),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"id\":4}",
                 dangerous[i]);
        zassert_ok(dispatch(request, &capture));
        zassert_true(response_has(&capture, "\"code\":403"));
        zassert_true(response_has(&capture, "\"jsonrpc\":\"2.0\""));
        zassert_false(response_has(&capture, "\"method\":"));
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
    zassert_ok(dispatch("{\"jsonrpc\":\"2.0\",\"method\":\"sys.version\","
                        "\"params\":null}", &capture));
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
    static const char *const number_ids[] = {"1e2", "1.5", "-0.25e+3"};

    for (size_t i = 0U; i < ARRAY_SIZE(number_ids); i++) {
        char request[96];

        snprintk(request, sizeof(request), "{\"m\":\"sys.version\",\"id\":%s}",
                 number_ids[i]);
        zassert_ok(dispatch(request, &capture));
        snprintk(request, sizeof(request), "\"id\":%s", number_ids[i]);
        zassert_true(response_has(&capture, request));

        snprintk(request, sizeof(request),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"sys.version\","
                 "\"id\":%s}", number_ids[i]);
        zassert_ok(dispatch(request, &capture));
        snprintk(request, sizeof(request), "\"id\":%s", number_ids[i]);
        zassert_true(response_has(&capture, request));
        zassert_true(response_has(&capture, "\"jsonrpc\":\"2.0\""));
    }

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
        "{\"m\":\"sys.version\",\"jsonrpc\":\"2.0\",\"id\":1}",
        "{\"m\":\"sys.version\",\"params\":null,\"id\":1}",
        "{\"method\":\"sys.version\",\"p\":null,\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"sys.version\",\"p\":null,"
        "\"id\":1}",
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

ZTEST(rpc, test_dynamic_json_strings_are_bounded_escaped_and_utf8_checked)
{
    static const char special[] = {'v', '"', '\\', '\b', '\f', '\n', '\r', '\t',
                                   0x01, (char)0xE4, (char)0xB8, (char)0xAD, '\0'};
    static const uint8_t want[] =
        "\"v\\\"\\\\\\b\\f\\n\\r\\t\\u0001\xE4\xB8\xAD\"";
    static const char invalid_utf8[] = {(char)0xC0, (char)0xAF, '\0'};
    static const char unterminated[] = {'x', 'y'};
    char too_long[CODEX_JSON_MAX_SIZE + 1U];
    struct capture capture;

    memset(&capture, 0, sizeof(capture));
    zassert_ok(codex_rpc_test_emit_json_cstr(special, sizeof(special), capture_emit,
                                             &capture));
    zassert_equal(capture.count, 1U);
    zassert_equal(capture.len, sizeof(want) - 1U);
    zassert_mem_equal(capture.json, want, sizeof(want) - 1U);

    memset(&capture, 0, sizeof(capture));
    zassert_equal(codex_rpc_test_emit_json_cstr(invalid_utf8, sizeof(invalid_utf8),
                                               capture_emit, &capture), -EINVAL);
    zassert_equal(capture.count, 0U);
    zassert_equal(codex_rpc_test_emit_json_cstr(unterminated, sizeof(unterminated),
                                               capture_emit, &capture), -EINVAL);
    zassert_equal(capture.count, 0U);

    memset(too_long, 'a', sizeof(too_long));
    too_long[sizeof(too_long) - 1U] = '\0';
    zassert_equal(codex_rpc_test_emit_json_cstr(too_long, sizeof(too_long),
                                               capture_emit, &capture), -EMSGSIZE);
    zassert_equal(capture.count, 0U);
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

ZTEST_SUITE(rpc, NULL, NULL, before, NULL, NULL);
