#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <codex/descriptor.h>
#include <codex/framing.h>

#define TEST_JSON_MAX 1024U
#define TEST_MAX_REPORTS 32U

struct captured_json {
    enum codex_transport transport;
    enum codex_channel channel;
    uint8_t data[TEST_JSON_MAX];
    size_t len;
};

static struct captured_json captured[8];
static size_t captured_count;
static uint32_t test_generation;
static uint8_t emitted[TEST_MAX_REPORTS][CODEX_VENDOR_PAYLOAD_SIZE];
static size_t emitted_count;
static size_t fail_emit_at;

extern bool codex_framing_test_canaries_intact(void);

struct corpus_case {
    const char *name;
    const uint8_t *data;
    size_t len;
    bool valid;
};

#include "corpus_data.h"

static void capture_json(enum codex_transport transport, enum codex_channel channel,
                         const uint8_t *json, size_t len)
{
    zassert_true(captured_count < ARRAY_SIZE(captured));
    zassert_true(len <= sizeof(captured[0].data));
    captured[captured_count].transport = transport;
    captured[captured_count].channel = channel;
    captured[captured_count].len = len;
    memcpy(captured[captured_count].data, json, len);
    captured_count++;
}

static int capture_report(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE], void *context)
{
    (void)context;
    if (emitted_count == fail_emit_at) {
        return -EIO;
    }
    zassert_true(emitted_count < ARRAY_SIZE(emitted));
    memcpy(emitted[emitted_count++], payload, CODEX_VENDOR_PAYLOAD_SIZE);
    return 0;
}

static int ingest(enum codex_transport transport, enum codex_channel channel,
                  uint32_t generation, const uint8_t *fragment, size_t len)
{
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE] = {0};

    zassert_true(len <= CODEX_FRAGMENT_MAX);
    payload[0] = (uint8_t)channel;
    payload[1] = (uint8_t)len;
    if (len > 0U) {
        memcpy(&payload[2], fragment, len);
    }
    return codex_framing_ingest(transport, payload, generation, capture_json);
}

static int ingest_json(enum codex_transport transport, enum codex_channel channel,
                       uint32_t generation, const uint8_t *json, size_t len)
{
    size_t offset = 0U;
    int err = 0;

    while (len - offset > CODEX_FRAGMENT_MAX) {
        err = ingest(transport, channel, generation, &json[offset], CODEX_FRAGMENT_MAX);
        if (err != 0) {
            return err;
        }
        offset += CODEX_FRAGMENT_MAX;
    }
    return ingest(transport, channel, generation, &json[offset], len - offset);
}

static void round_trip_encoded(const uint8_t *json, size_t len)
{
    emitted_count = 0U;
    captured_count = 0U;
    zassert_ok(codex_framing_encode(CODEX_CHANNEL_RPC, json, len,
                                    capture_report, NULL));
    for (size_t i = 0U; i < emitted_count; i++) {
        zassert_ok(codex_framing_ingest(CODEX_TRANSPORT_USB, emitted[i],
                                        test_generation, capture_json));
    }
    zassert_equal(captured_count, 1U);
    zassert_equal(captured[0].len, len);
    zassert_mem_equal(captured[0].data, json, len);
}

static void reset_case(void)
{
    uint8_t empty[CODEX_VENDOR_PAYLOAD_SIZE] = {CODEX_CHANNEL_RPC, 0U};

    captured_count = 0U;
    emitted_count = 0U;
    fail_emit_at = SIZE_MAX;
    test_generation++;
    (void)codex_framing_ingest(CODEX_TRANSPORT_USB, empty, test_generation, capture_json);
    (void)codex_framing_ingest(CODEX_TRANSPORT_BLE, empty, test_generation, capture_json);
    empty[0] = CODEX_CHANNEL_DEBUG;
    (void)codex_framing_ingest(CODEX_TRANSPORT_USB, empty, test_generation, capture_json);
    (void)codex_framing_ingest(CODEX_TRANSPORT_BLE, empty, test_generation, capture_json);
    captured_count = 0U;
}

static void *framing_setup(void)
{
    return NULL;
}

static void framing_before(void *fixture)
{
    (void)fixture;
    reset_case();
}

ZTEST(framing, test_single_and_fragmented_values_without_newline)
{
    static const uint8_t single[] = "{\"id\":1}";
    static const uint8_t fragmented[] =
        "{\"m\":\"device.status\",\"p\":{\"nested\":[true,false,null]},\"id\":2}";

    zassert_ok(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                           single, sizeof(single) - 1U));
    zassert_equal(captured_count, 1U);
    zassert_mem_equal(captured[0].data, single, sizeof(single) - 1U);

    zassert_ok(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                           fragmented, sizeof(fragmented) - 1U));
    zassert_equal(captured_count, 2U);
    zassert_mem_equal(captured[1].data, fragmented, sizeof(fragmented) - 1U);
}

ZTEST(framing, test_accepts_lf_and_crlf_but_callback_excludes_them)
{
    static const uint8_t lf[] = "[1,{\"x\":2}]\n";
    static const uint8_t crlf[] = "{\"x\":3}\r\n";

    zassert_ok(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                           lf, sizeof(lf) - 1U));
    zassert_ok(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                           crlf, sizeof(crlf) - 1U));
    zassert_equal(captured_count, 2U);
    zassert_equal(captured[0].len, sizeof(lf) - 2U);
    zassert_equal(captured[1].len, sizeof(crlf) - 3U);
}

ZTEST(framing, test_split_crlf_is_accepted_but_bare_cr_is_not_a_terminator)
{
    static const uint8_t json_cr[] = "{\"x\":4}\r";

    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                      json_cr, sizeof(json_cr) - 1U));
    zassert_equal(captured_count, 0U);
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                      (const uint8_t *)"\n", 1U));
    zassert_equal(captured_count, 1U);
    zassert_equal(captured[0].len, sizeof(json_cr) - 2U);

    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                      json_cr, sizeof(json_cr) - 1U));
    zassert_true(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                        NULL, 0U) < 0);
    zassert_equal(captured_count, 1U);
}

ZTEST(framing, test_fragment_lengths_zero_61_and_invalid_62)
{
    uint8_t full[CODEX_FRAGMENT_MAX];
    uint8_t invalid[CODEX_VENDOR_PAYLOAD_SIZE] = {CODEX_CHANNEL_RPC, 62U};

    full[0] = '"';
    memset(&full[1], 'x', sizeof(full) - 2U);
    full[sizeof(full) - 1U] = '"';
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation, full,
                      sizeof(full)));
    zassert_equal(captured_count, 1U);
    zassert_equal(captured[0].len, CODEX_FRAGMENT_MAX);
    zassert_true(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation, NULL, 0U) < 0);
    zassert_true(codex_framing_ingest(CODEX_TRANSPORT_USB, invalid, test_generation,
                                     capture_json) < 0);
    zassert_equal(captured_count, 1U);
}

ZTEST(framing, test_generation_change_resets_both_channels_only_on_that_transport)
{
    static const uint8_t rpc_a[] = "{\"usb\":\"old";
    static const uint8_t debug_a[] = "[\"debug";
    static const uint8_t ble_a[] = "{\"ble\":\"kept";
    static const uint8_t suffix[] = "\"}";
    uint32_t next_generation = test_generation + 1U;

    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                      rpc_a, sizeof(rpc_a) - 1U));
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_DEBUG, test_generation,
                      debug_a, sizeof(debug_a) - 1U));
    zassert_ok(ingest(CODEX_TRANSPORT_BLE, CODEX_CHANNEL_RPC, test_generation,
                      ble_a, sizeof(ble_a) - 1U));
    zassert_ok(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, next_generation,
                           (const uint8_t *)"{\"new\":1}", 9U));
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_DEBUG, next_generation,
                      (const uint8_t *)"\"]", 2U));
    zassert_true(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_DEBUG, next_generation,
                        NULL, 0U) < 0);
    zassert_ok(ingest(CODEX_TRANSPORT_BLE, CODEX_CHANNEL_RPC, test_generation,
                      suffix, sizeof(suffix) - 1U));

    zassert_equal(captured_count, 2U);
    zassert_equal(captured[0].transport, CODEX_TRANSPORT_USB);
    zassert_equal(captured[1].transport, CODEX_TRANSPORT_BLE);
    zassert_mem_equal(captured[1].data, "{\"ble\":\"kept\"}", 14U);
}

ZTEST(framing, test_transport_and_channel_buffers_are_independent)
{
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                      (const uint8_t *)"{\"a\":", 5U));
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_DEBUG, test_generation,
                      (const uint8_t *)"[1,", 3U));
    zassert_ok(ingest(CODEX_TRANSPORT_BLE, CODEX_CHANNEL_RPC, test_generation,
                      (const uint8_t *)"{\"b\":", 5U));
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_DEBUG, test_generation,
                      (const uint8_t *)"2]", 2U));
    zassert_ok(ingest(CODEX_TRANSPORT_BLE, CODEX_CHANNEL_RPC, test_generation,
                      (const uint8_t *)"3}", 2U));
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                      (const uint8_t *)"4}", 2U));
    zassert_equal(captured_count, 3U);
}

ZTEST(framing, test_rejects_bad_channel_padding_trailing_junk_and_multiple_values)
{
    uint8_t bad_channel[CODEX_VENDOR_PAYLOAD_SIZE] = {0x03, 2U, '{', '}'};
    uint8_t bad_padding[CODEX_VENDOR_PAYLOAD_SIZE] = {CODEX_CHANNEL_RPC, 2U, '{', '}', 1U};

    zassert_true(codex_framing_ingest(CODEX_TRANSPORT_USB, bad_channel, test_generation,
                                     capture_json) < 0);
    zassert_true(codex_framing_ingest(CODEX_TRANSPORT_USB, bad_padding, test_generation,
                                     capture_json) < 0);
    zassert_true(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                            (const uint8_t *)"{}x", 3U) < 0);
    zassert_true(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                            (const uint8_t *)"{}[]", 4U) < 0);
    zassert_equal(captured_count, 0U);
}

ZTEST(framing, test_utf8_escape_and_depth_validation)
{
    static const uint8_t valid[] =
        "{\"utf8\":\"\xE4\xB8\xAD\xE6\x96\x87\",\"escape\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u20ac\"}";
    static const uint8_t overlong[] = {'{','\"','x','\"',':','\"',0xC0,0xAF,'\"','}'};
    static const uint8_t surrogate_utf8[] = {'{','\"','x','\"',':','\"',0xED,0xA0,0x80,'\"','}'};
    static const uint8_t too_high[] = {'{','\"','x','\"',':','\"',0xF4,0x90,0x80,0x80,'\"','}'};
    static const uint8_t truncated[] = {'{','\"','x','\"',':','\"',0xE2,0x82};
    uint8_t nested[70];

    zassert_ok(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                           valid, sizeof(valid) - 1U));
    zassert_true(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                            overlong, sizeof(overlong)) < 0);
    zassert_true(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                            surrogate_utf8, sizeof(surrogate_utf8)) < 0);
    zassert_true(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                            too_high, sizeof(too_high)) < 0);
    zassert_ok(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                           truncated, sizeof(truncated)));
    zassert_true(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                        NULL, 0U) < 0);
    zassert_equal(captured_count, 1U);

    for (size_t i = 0U; i < CODEX_JSON_MAX_DEPTH; i++) {
        nested[i] = '[';
        nested[CODEX_JSON_MAX_DEPTH * 2U - 1U - i] = ']';
    }
    zassert_ok(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                           nested, CODEX_JSON_MAX_DEPTH * 2U));
    for (size_t i = 0U; i < CODEX_JSON_MAX_DEPTH + 1U; i++) {
        nested[i] = '[';
        nested[(CODEX_JSON_MAX_DEPTH + 1U) * 2U - 1U - i] = ']';
    }
    zassert_true(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                            nested, CODEX_JSON_MAX_DEPTH * 2U + 2U) < 0);
}

ZTEST(framing, test_accepts_exactly_1024_bytes_and_resets_on_overflow)
{
    uint8_t maximum[TEST_JSON_MAX];
    uint8_t over[TEST_JSON_MAX + 1U];

    maximum[0] = '"';
    memset(&maximum[1], 'a', sizeof(maximum) - 2U);
    maximum[sizeof(maximum) - 1U] = '"';
    memcpy(over, maximum, sizeof(maximum));
    over[sizeof(maximum) - 1U] = 'a';
    over[sizeof(maximum)] = '"';

    zassert_ok(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                           maximum, sizeof(maximum)));
    zassert_equal(captured_count, 1U);
    zassert_equal(captured[0].len, sizeof(maximum));
    zassert_true(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                            over, sizeof(over)) < 0);
    zassert_ok(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                           (const uint8_t *)"{}", 2U));
    zassert_equal(captured_count, 2U);
}

ZTEST(framing, test_encode_validates_and_round_trips_with_crlf_and_zero_padding)
{
    uint8_t json[62];

    json[0] = '"';
    memset(&json[1], 'a', sizeof(json) - 2U);
    json[sizeof(json) - 1U] = '"';
    zassert_ok(codex_framing_encode(CODEX_CHANNEL_RPC, json, sizeof(json), capture_report, NULL));
    zassert_equal(emitted_count, 2U);
    zassert_equal(emitted[0][0], CODEX_CHANNEL_RPC);
    zassert_equal(emitted[0][1], CODEX_FRAGMENT_MAX);
    zassert_equal(emitted[1][1], 3U);
    zassert_equal(emitted[1][2], '"');
    zassert_equal(emitted[1][3], '\r');
    zassert_equal(emitted[1][4], '\n');
    for (size_t i = 5U; i < CODEX_VENDOR_PAYLOAD_SIZE; i++) {
        zassert_equal(emitted[1][i], 0U);
    }

    captured_count = 0U;
    for (size_t i = 0U; i < emitted_count; i++) {
        zassert_ok(codex_framing_ingest(CODEX_TRANSPORT_USB, emitted[i], test_generation,
                                        capture_json));
    }
    zassert_equal(captured_count, 1U);
    zassert_mem_equal(captured[0].data, json, sizeof(json));
}

ZTEST(framing, test_encode_exact_61_and_122_byte_json_round_trip)
{
    uint8_t json[TEST_JSON_MAX];
    static const size_t lengths[] = {61U, 122U, TEST_JSON_MAX};

    for (size_t case_index = 0U; case_index < ARRAY_SIZE(lengths); case_index++) {
        size_t len = lengths[case_index];

        emitted_count = 0U;
        captured_count = 0U;
        json[0] = '"';
        memset(&json[1], 'q', len - 2U);
        json[len - 1U] = '"';
        zassert_ok(codex_framing_encode(CODEX_CHANNEL_RPC, json, len,
                                        capture_report, NULL));
        zassert_true(emitted_count >= 2U);
        zassert_true(emitted[emitted_count - 1U][1] >= 3U);
        zassert_equal(emitted[emitted_count - 1U]
                             [emitted[emitted_count - 1U][1]], '\r');
        zassert_equal(emitted[emitted_count - 1U]
                             [emitted[emitted_count - 1U][1] + 1U], '\n');
        for (size_t i = 0U; i < emitted_count; i++) {
            zassert_ok(codex_framing_ingest(CODEX_TRANSPORT_USB, emitted[i],
                                            test_generation, capture_json));
        }
        zassert_equal(captured_count, 1U);
        zassert_equal(captured[0].len, len);
        zassert_mem_equal(captured[0].data, json, len);
    }
}

ZTEST(framing, test_fragmented_top_level_numbers_wait_for_a_delimiter)
{
    uint8_t integer[122];
    uint8_t decimal[100];
    uint8_t exponent[100];

    memset(integer, '7', sizeof(integer));
    memset(decimal, '4', sizeof(decimal));
    decimal[60] = '.';
    memset(exponent, '5', sizeof(exponent));
    exponent[60] = 'e';
    exponent[61] = '+';

    round_trip_encoded(integer, 100U);
    round_trip_encoded(integer, sizeof(integer));
    round_trip_encoded(decimal, sizeof(decimal));
    round_trip_encoded(exponent, sizeof(exponent));

    captured_count = 0U;
    zassert_ok(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC,
                           test_generation, integer, 100U));
    zassert_equal(captured_count, 0U);
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                      NULL, 0U));
    zassert_equal(captured_count, 1U);
    zassert_equal(captured[0].len, 100U);
}

ZTEST(framing, test_internal_json_line_breaks_at_fragment_boundary_are_preserved)
{
    static const uint8_t endings[][2] = {{'\n', 0U}, {'\r', 0U}, {'\r', '\n'}};
    uint8_t json[80];

    for (size_t case_index = 0U; case_index < ARRAY_SIZE(endings); case_index++) {
        size_t ending_len = endings[case_index][1] == 0U ? 1U : 2U;
        size_t prefix_len = CODEX_FRAGMENT_MAX - ending_len;
        size_t len;

        memcpy(json, "{\"a\":", 5U);
        memset(&json[5], ' ', prefix_len - 5U);
        memcpy(&json[prefix_len], endings[case_index], ending_len);
        json[CODEX_FRAGMENT_MAX] = '1';
        json[CODEX_FRAGMENT_MAX + 1U] = '}';
        len = CODEX_FRAGMENT_MAX + 2U;

        round_trip_encoded(json, len);
    }
}

ZTEST(framing, test_stale_and_half_range_generations_are_rejected_without_state_change)
{
    uint32_t current = test_generation + 1U;
    uint32_t stale = test_generation;
    uint32_t half_range = current + UINT32_C(0x80000000);

    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, current,
                      (const uint8_t *)"{\"kept\":", 8U));
    zassert_equal(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, stale,
                              (const uint8_t *)"{\"stale\":1}", 11U), -ESTALE);
    zassert_equal(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, stale,
                         (const uint8_t *)"{\"multi\":", 9U), -ESTALE);
    zassert_equal(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, stale,
                         (const uint8_t *)"2}", 2U), -ESTALE);
    zassert_equal(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, half_range,
                              (const uint8_t *)"{}", 2U), -ESTALE);
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, current,
                      (const uint8_t *)"true}", 5U));
    zassert_equal(captured_count, 1U);
    zassert_mem_equal(captured[0].data, "{\"kept\":true}", 13U);
}

ZTEST(framing, test_generation_wrap_from_uint32_max_to_zero_is_newer)
{
    uint32_t current = test_generation;
    uint32_t remaining = UINT32_MAX - current;

    while (remaining > 0U) {
        uint32_t step = remaining > INT32_MAX ? INT32_MAX : remaining;

        current += step;
        zassert_ok(ingest(CODEX_TRANSPORT_BLE, CODEX_CHANNEL_RPC, current,
                          (const uint8_t *)"{", 1U));
        remaining -= step;
    }
    zassert_ok(ingest_json(CODEX_TRANSPORT_BLE, CODEX_CHANNEL_RPC, 0U,
                           (const uint8_t *)"{\"wrapped\":true}", 16U));
    zassert_equal(captured_count, 1U);
}

ZTEST(framing, test_invalid_channel_does_not_clear_known_channel_partials)
{
    uint8_t invalid[CODEX_VENDOR_PAYLOAD_SIZE] = {0x7FU, 2U, '{', '}'};

    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                      (const uint8_t *)"{\"r\":", 5U));
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_DEBUG, test_generation,
                      (const uint8_t *)"[1,", 3U));
    zassert_equal(codex_framing_ingest(CODEX_TRANSPORT_USB, invalid,
                                       test_generation, capture_json), -EINVAL);
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                      (const uint8_t *)"2}", 2U));
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_DEBUG, test_generation,
                      (const uint8_t *)"3]", 2U));
    zassert_equal(captured_count, 2U);
}

ZTEST(framing, test_generation_is_processed_before_invalid_channel)
{
    uint8_t invalid[CODEX_VENDOR_PAYLOAD_SIZE] = {0x7FU, 2U, '{', '}'};
    uint32_t old_generation = test_generation;
    uint32_t new_generation = test_generation + 1U;

    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, old_generation,
                      (const uint8_t *)"{\"old\":", 7U));
    zassert_equal(codex_framing_ingest(CODEX_TRANSPORT_USB, invalid,
                                       new_generation, capture_json), -EINVAL);
    zassert_equal(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, old_generation,
                         (const uint8_t *)"1}", 2U), -ESTALE);
    zassert_equal(codex_framing_ingest(CODEX_TRANSPORT_USB, invalid,
                                       old_generation, capture_json), -ESTALE);
    zassert_ok(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, new_generation,
                           (const uint8_t *)"{\"new\":1}", 9U));
    zassert_equal(captured_count, 1U);

    captured_count = 0U;
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_DEBUG, new_generation,
                      (const uint8_t *)"[1,", 3U));
    zassert_equal(codex_framing_ingest(CODEX_TRANSPORT_USB, invalid,
                                       old_generation, capture_json), -ESTALE);
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_DEBUG, new_generation,
                      (const uint8_t *)"2]", 2U));
    zassert_equal(captured_count, 1U);
}

ZTEST(framing, test_trailing_space_and_tab_are_preserved)
{
    static const uint8_t space[] = "{} ";
    static const uint8_t tab[] = "{}\t";
    uint8_t boundary[CODEX_FRAGMENT_MAX + 3U];
    uint8_t exact_full[CODEX_FRAGMENT_MAX];

    round_trip_encoded(space, sizeof(space) - 1U);
    round_trip_encoded(tab, sizeof(tab) - 1U);

    captured_count = 0U;
    zassert_ok(ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC,
                           test_generation, space, sizeof(space) - 1U));
    zassert_equal(captured_count, 1U);
    zassert_mem_equal(captured[0].data, space, sizeof(space) - 1U);

    memcpy(exact_full, "{}", 2U);
    memset(&exact_full[2], ' ', sizeof(exact_full) - 2U);
    captured_count = 0U;
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                      exact_full, sizeof(exact_full)));
    zassert_equal(captured_count, 0U);
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, test_generation,
                      NULL, 0U));
    zassert_equal(captured_count, 1U);
    zassert_equal(captured[0].len, sizeof(exact_full));
    zassert_mem_equal(captured[0].data, exact_full, sizeof(exact_full));

    memcpy(boundary, "{}", 2U);
    memset(&boundary[2], ' ', CODEX_FRAGMENT_MAX - 2U);
    boundary[CODEX_FRAGMENT_MAX] = '\t';
    boundary[CODEX_FRAGMENT_MAX + 1U] = ' ';
    boundary[CODEX_FRAGMENT_MAX + 2U] = '\t';
    round_trip_encoded(boundary, sizeof(boundary));
}

static void ingest_1024_json_then_cr(uint32_t generation, const uint8_t json[TEST_JSON_MAX])
{
    size_t offset = 0U;

    while (TEST_JSON_MAX - offset > 48U) {
        zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, generation,
                          &json[offset], CODEX_FRAGMENT_MAX));
        offset += CODEX_FRAGMENT_MAX;
    }
    uint8_t tail[49];

    zassert_equal(TEST_JSON_MAX - offset, 48U);
    memcpy(tail, &json[offset], 48U);
    tail[48] = '\r';
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, generation,
                      tail, sizeof(tail)));
}

ZTEST(framing, test_1024_byte_json_accepts_split_crlf_without_buffer_overflow)
{
    uint8_t maximum[TEST_JSON_MAX];
    uint32_t generation = test_generation;

    maximum[0] = '"';
    memset(&maximum[1], 'm', sizeof(maximum) - 2U);
    maximum[sizeof(maximum) - 1U] = '"';

    ingest_1024_json_then_cr(generation, maximum);
    zassert_equal(captured_count, 0U);
    zassert_true(codex_framing_test_canaries_intact());
    zassert_ok(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, generation,
                      (const uint8_t *)"\n", 1U));
    zassert_equal(captured_count, 1U);
    zassert_equal(captured[0].len, sizeof(maximum));
    zassert_mem_equal(captured[0].data, maximum, sizeof(maximum));
    zassert_true(codex_framing_test_canaries_intact());

    captured_count = 0U;
    ingest_1024_json_then_cr(generation, maximum);
    zassert_true(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, generation,
                        NULL, 0U) < 0);
    zassert_equal(captured_count, 0U);
    zassert_true(codex_framing_test_canaries_intact());

    ingest_1024_json_then_cr(generation, maximum);
    zassert_true(ingest(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC, generation,
                        (const uint8_t *)"x", 1U) < 0);
    zassert_equal(captured_count, 0U);
    zassert_true(codex_framing_test_canaries_intact());
}

ZTEST(framing, test_encode_rejects_inputs_and_stops_at_first_emit_error)
{
    uint8_t long_json[100] = {'"'};
    uint8_t oversized[TEST_JSON_MAX + 1U] = {'"'};

    memset(&long_json[1], 'x', sizeof(long_json) - 2U);
    long_json[sizeof(long_json) - 1U] = '"';
    zassert_equal(codex_framing_encode(CODEX_CHANNEL_RPC, NULL, 1U, capture_report, NULL),
                  -EINVAL);
    zassert_equal(codex_framing_encode((enum codex_channel)3, (const uint8_t *)"{}", 2U,
                                      capture_report, NULL), -EINVAL);
    zassert_equal(codex_framing_encode(CODEX_CHANNEL_RPC, (const uint8_t *)"{}x", 3U,
                                      capture_report, NULL), -EINVAL);
    zassert_equal(codex_framing_encode(CODEX_CHANNEL_RPC, (const uint8_t *)"{}", 2U,
                                      NULL, NULL), -EINVAL);
    zassert_equal(codex_framing_encode(CODEX_CHANNEL_RPC, oversized, sizeof(oversized),
                                      capture_report, NULL), -EMSGSIZE);
    fail_emit_at = 1U;
    zassert_equal(codex_framing_encode(CODEX_CHANNEL_RPC, long_json, sizeof(long_json),
                                      capture_report, NULL), -EIO);
    zassert_equal(emitted_count, 1U);
}

ZTEST(framing, test_fuzz_corpus_is_executed)
{
    for (size_t i = 0U; i < ARRAY_SIZE(corpus_cases); i++) {
        size_t before = captured_count;
        int err = ingest_json(CODEX_TRANSPORT_USB, CODEX_CHANNEL_RPC,
                              test_generation, corpus_cases[i].data,
                              corpus_cases[i].len);

        if (corpus_cases[i].valid) {
            zassert_ok(err, "%s", corpus_cases[i].name);
            zassert_equal(captured_count, before + 1U, "%s",
                          corpus_cases[i].name);
        } else {
            zassert_true(err < 0, "%s", corpus_cases[i].name);
            zassert_equal(captured_count, before, "%s", corpus_cases[i].name);
        }
    }
}

ZTEST_SUITE(framing, NULL, framing_setup, framing_before, NULL, NULL);
