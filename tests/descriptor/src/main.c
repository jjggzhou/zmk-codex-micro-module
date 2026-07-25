#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <codex/descriptor.h>

enum hid_main_item {
    HID_INPUT = 0x80,
    HID_OUTPUT = 0x90,
    HID_COLLECTION = 0xA0,
};

struct sha256_context {
    uint32_t state[8];
};

static uint32_t rotate_right(uint32_t value, uint32_t amount)
{
    return (value >> amount) | (value << (32U - amount));
}

static void sha256_compress(struct sha256_context *context, const uint8_t block[64])
{
    static const uint32_t constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
        0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
        0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
        0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };
    uint32_t words[64];
    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    uint32_t e = context->state[4];
    uint32_t f = context->state[5];
    uint32_t g = context->state[6];
    uint32_t h = context->state[7];

    for (size_t index = 0U; index < 16U; index++) {
        words[index] = ((uint32_t)block[index * 4U] << 24U) |
                       ((uint32_t)block[index * 4U + 1U] << 16U) |
                       ((uint32_t)block[index * 4U + 2U] << 8U) |
                       block[index * 4U + 3U];
    }
    for (size_t index = 16U; index < 64U; index++) {
        uint32_t sigma0 = rotate_right(words[index - 15U], 7U) ^
                          rotate_right(words[index - 15U], 18U) ^ (words[index - 15U] >> 3U);
        uint32_t sigma1 = rotate_right(words[index - 2U], 17U) ^
                          rotate_right(words[index - 2U], 19U) ^ (words[index - 2U] >> 10U);

        words[index] = words[index - 16U] + sigma0 + words[index - 7U] + sigma1;
    }
    for (size_t index = 0U; index < 64U; index++) {
        uint32_t sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t temporary1 = h + sum1 + choice + constants[index] + words[index];
        uint32_t sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void sha256(const uint8_t *data, size_t data_size, uint8_t digest[32])
{
    struct sha256_context context = {
        .state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                  0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U},
    };
    uint8_t final_block[64] = {0U};
    uint64_t bit_length = (uint64_t)data_size * 8U;

    while (data_size >= sizeof(final_block)) {
        sha256_compress(&context, data);
        data += sizeof(final_block);
        data_size -= sizeof(final_block);
    }

    memcpy(final_block, data, data_size);
    final_block[data_size] = 0x80U;
    if (data_size >= 56U) {
        sha256_compress(&context, final_block);
        memset(final_block, 0, sizeof(final_block));
    }
    for (size_t index = 0U; index < 8U; index++) {
        final_block[63U - index] = (uint8_t)(bit_length >> (index * 8U));
    }
    sha256_compress(&context, final_block);

    for (size_t index = 0U; index < 8U; index++) {
        digest[index * 4U] = (uint8_t)(context.state[index] >> 24U);
        digest[index * 4U + 1U] = (uint8_t)(context.state[index] >> 16U);
        digest[index * 4U + 2U] = (uint8_t)(context.state[index] >> 8U);
        digest[index * 4U + 3U] = (uint8_t)context.state[index];
    }
}

static size_t hid_item_size(uint8_t prefix)
{
    static const size_t sizes[] = {0U, 1U, 2U, 4U};

    return sizes[prefix & 0x03U];
}

static uint32_t hid_item_value(const uint8_t *data, size_t size)
{
    uint32_t value = 0U;

    for (size_t index = 0U; index < size; index++) {
        value |= (uint32_t)data[index] << (index * 8U);
    }

    return value;
}

static size_t count_report_id(const uint8_t *descriptor, size_t descriptor_size,
                              uint8_t report_id)
{
    size_t count = 0U;

    for (size_t index = 0U; index < descriptor_size;) {
        uint8_t prefix = descriptor[index++];
        size_t size;

        if (prefix == 0xFEU) {
            zassert_true(index + 2U <= descriptor_size, "truncated long HID item");
            size = descriptor[index];
            index += 2U;
        } else {
            size = hid_item_size(prefix);
        }
        zassert_true(index + size <= descriptor_size, "truncated HID item");
        if (prefix == 0x85U && size == 1U && descriptor[index] == report_id) {
            count++;
        }
        index += size;
    }

    return count;
}

static uint32_t report_payload_bits_in(const uint8_t *descriptor, size_t descriptor_size,
                                       uint8_t wanted_report_id, enum hid_main_item wanted_type)
{
    uint8_t report_id = 0U;
    uint32_t report_size = 0U;
    uint32_t report_count = 0U;
    uint32_t bits = 0U;

    for (size_t index = 0U; index < descriptor_size;) {
        uint8_t prefix = descriptor[index++];
        size_t size;
        uint32_t value;

        if (prefix == 0xFEU) {
            zassert_true(index + 2U <= descriptor_size, "truncated long HID item");
            size = descriptor[index];
            index += 2U;
        } else {
            size = hid_item_size(prefix);
        }
        zassert_true(index + size <= descriptor_size, "truncated HID item");
        value = hid_item_value(&descriptor[index], size);

        if (prefix == 0x85U) {
            report_id = (uint8_t)value;
        } else if (prefix == 0x75U) {
            report_size = value;
        } else if (prefix == 0x95U) {
            report_count = value;
        } else if ((prefix & 0xFCU) == wanted_type && report_id == wanted_report_id) {
            bits += report_size * report_count;
        }
        index += size;
    }

    return bits;
}

static uint32_t report_payload_bits(uint8_t wanted_report_id, enum hid_main_item wanted_type)
{
    return report_payload_bits_in(codex_usb_report_descriptor, codex_usb_report_descriptor_size(),
                                  wanted_report_id, wanted_type);
}

static size_t top_level_application_collections_in(const uint8_t *descriptor, size_t descriptor_size)
{
    size_t count = 0U;
    size_t depth = 0U;

    for (size_t index = 0U; index < descriptor_size;) {
        uint8_t prefix = descriptor[index++];
        size_t size;

        if (prefix == 0xFEU) {
            zassert_true(index + 2U <= descriptor_size, "truncated long HID item");
            size = descriptor[index];
            index += 2U;
        } else {
            size = hid_item_size(prefix);
        }
        zassert_true(index + size <= descriptor_size, "truncated HID item");

        if ((prefix & 0xFCU) == HID_COLLECTION) {
            if (depth == 0U && size == 1U && descriptor[index] == 0x01U) {
                count++;
            }
            depth++;
        } else if (prefix == 0xC0U) {
            zassert_true(depth > 0U, "unbalanced HID collection");
            depth--;
        }
        index += size;
    }

    zassert_equal(depth, 0U, "unbalanced HID collection");
    return count;
}

static size_t top_level_application_collections(void)
{
    return top_level_application_collections_in(codex_usb_report_descriptor,
                                                codex_usb_report_descriptor_size());
}

ZTEST(codex_descriptor, test_original_descriptor_structure)
{
    zassert_equal(codex_usb_report_descriptor_size(), 275U);
    zassert_equal(count_report_id(codex_usb_report_descriptor, 275U, 0x06U), 1U);
    zassert_equal(report_payload_bits(0x06U, HID_INPUT), 63U * 8U);
    zassert_equal(report_payload_bits(0x06U, HID_OUTPUT), 63U * 8U);
    zassert_equal(top_level_application_collections(), 5U);
}

ZTEST(codex_descriptor, test_original_descriptor_hash)
{
    static const uint8_t expected_sha256[32] = {
        0x92U, 0x57U, 0xd7U, 0x36U, 0x1fU, 0x9cU, 0x78U, 0x4eU,
        0x0fU, 0xc0U, 0xb2U, 0x60U, 0xbbU, 0xacU, 0x0fU, 0xeaU,
        0xddU, 0x49U, 0xbfU, 0x79U, 0xcbU, 0xb6U, 0x20U, 0x2dU,
        0x6cU, 0x41U, 0x56U, 0x0cU, 0xbaU, 0xe9U, 0x6fU, 0xb6U,
    };
    uint8_t actual_sha256[sizeof(expected_sha256)];

    sha256(codex_usb_report_descriptor, codex_usb_report_descriptor_size(), actual_sha256);
    zassert_mem_equal(actual_sha256, expected_sha256, sizeof(expected_sha256));
}

ZTEST_SUITE(codex_descriptor, NULL, NULL, NULL, NULL, NULL);
