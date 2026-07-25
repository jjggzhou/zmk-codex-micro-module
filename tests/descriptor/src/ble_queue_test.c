#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <codex/ble_hids.h>

#include "../../../src/transport/ble_queue.h"

struct fake_connection {
    uint8_t id;
    size_t references;
};

static size_t output_count;
static uint8_t last_output[CODEX_VENDOR_PAYLOAD_SIZE];
static struct codex_ble_source_token last_token;

static void *fake_ref(void *connection)
{
    struct fake_connection *fake = connection;

    fake->references++;
    return fake;
}

static void fake_unref(void *connection)
{
    struct fake_connection *fake = connection;

    zassert_true(fake->references > 0U);
    fake->references--;
}

/* This is the same strong Task-7 seam reached directly by the GATT write callback. */
int codex_ble_vendor_output_received_with_token(
    const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
    const struct codex_ble_source_token *token)
{
    output_count++;
    memcpy(last_output, payload, sizeof(last_output));
    last_token = *token;
    return 0;
}

static void strong_output_emit(const uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE],
                               const struct codex_ble_source_token *token, void *context)
{
    ARG_UNUSED(context);
    codex_ble_vendor_output_received_with_token(payload, token);
}

static void reset_output(void)
{
    output_count = 0U;
    memset(last_output, 0, sizeof(last_output));
    memset(&last_token, 0, sizeof(last_token));
}

ZTEST(codex_ble_queue, test_current_connection_reaches_strong_output_seam_once)
{
    struct fake_connection connection_a = {.id = 1};
    struct codex_ble_owned_item item;
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE];

    memset(payload, 0x6a, sizeof(payload));
    reset_output();
    zassert_ok(codex_ble_owned_item_capture(&item, &connection_a, 2U, 7U, payload, fake_ref));
    zassert_equal(connection_a.references, 1U);
    zassert_true(codex_ble_owned_item_dispatch(&item, &connection_a, 2U, 7U,
                                              strong_output_emit, NULL, fake_unref));
    zassert_equal(output_count, 1U);
    zassert_mem_equal(last_output, payload, sizeof(payload));
    zassert_equal(last_token.profile_index, 2U);
    zassert_equal(last_token.connection_generation, 7U);
    zassert_equal(connection_a.references, 0U);
}

ZTEST(codex_ble_queue, test_profile_switch_never_redirects_a_queued_output_to_connection_b)
{
    struct fake_connection connection_a = {.id = 1};
    struct fake_connection connection_b = {.id = 2};
    struct codex_ble_owned_item item;
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE] = {0};

    reset_output();
    zassert_ok(codex_ble_owned_item_capture(&item, &connection_a, 0U, 11U, payload, fake_ref));
    zassert_false(codex_ble_owned_item_dispatch(&item, &connection_b, 1U, 11U,
                                               strong_output_emit, NULL, fake_unref));
    zassert_equal(output_count, 0U);
    zassert_equal(connection_a.references, 0U);
    zassert_equal(connection_b.references, 0U);
}

ZTEST(codex_ble_queue, test_disconnect_generation_invalidates_queued_output)
{
    struct fake_connection connection_a = {.id = 1};
    struct codex_ble_owned_item item;
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE] = {0};

    reset_output();
    zassert_ok(codex_ble_owned_item_capture(&item, &connection_a, 0U, 17U, payload, fake_ref));
    zassert_false(codex_ble_owned_item_dispatch(&item, NULL, UINT8_MAX, 18U,
                                               strong_output_emit, NULL, fake_unref));
    zassert_equal(output_count, 0U);
    zassert_equal(connection_a.references, 0U);
}

ZTEST(codex_ble_queue, test_queue_full_cleanup_releases_owned_connection)
{
    struct fake_connection connection_a = {.id = 1};
    struct codex_ble_owned_item item;
    uint8_t payload[CODEX_VENDOR_PAYLOAD_SIZE] = {0};

    zassert_ok(codex_ble_owned_item_capture(&item, &connection_a, 0U, 1U, payload, fake_ref));
    /* Mirrors the production k_msgq_put() failure path. */
    codex_ble_owned_item_release(&item, fake_unref);
    zassert_equal(connection_a.references, 0U);
    zassert_is_null(item.connection);
}

ZTEST_SUITE(codex_ble_queue, NULL, NULL, NULL, NULL, NULL);
