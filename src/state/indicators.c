#include <codex/state.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/toolchain.h>

#include "state_internal.h"

static atomic_t current_bits = ATOMIC_INIT(CODEX_INDICATOR_TOP);

uint8_t codex_layer_indicator_bits(uint8_t layer)
{
    static const uint8_t bits[CODEX_LAYER_COUNT] = {
        CODEX_INDICATOR_TOP,
        CODEX_INDICATOR_MIDDLE,
        CODEX_INDICATOR_BOTTOM,
        CODEX_INDICATOR_TOP | CODEX_INDICATOR_MIDDLE,
        CODEX_INDICATOR_MIDDLE | CODEX_INDICATOR_BOTTOM,
        CODEX_INDICATOR_TOP | CODEX_INDICATOR_MIDDLE | CODEX_INDICATOR_BOTTOM,
    };

    return layer < CODEX_LAYER_COUNT ? bits[layer] : 0U;
}

uint8_t codex_connection_indicator_bits(enum codex_connection_choice choice)
{
    static const uint8_t bits[CODEX_CONNECTION_CHOICE_COUNT] = {
        CODEX_INDICATOR_TOP,
        CODEX_INDICATOR_MIDDLE,
        CODEX_INDICATOR_BOTTOM,
        CODEX_INDICATOR_TOP | CODEX_INDICATOR_MIDDLE | CODEX_INDICATOR_BOTTOM,
    };

    return (unsigned int)choice < CODEX_CONNECTION_CHOICE_COUNT ? bits[choice]
                                                                 : 0U;
}

__weak void codex_indicator_sink(uint8_t bits) { ARG_UNUSED(bits); }

void codex_indicators_render(uint8_t bits)
{
    atomic_set(&current_bits, bits);
    codex_indicator_sink(bits);
}

uint8_t codex_indicator_bits(void) { return (uint8_t)atomic_get(&current_bits); }

void codex_indicators_reset(void) { atomic_set(&current_bits, CODEX_INDICATOR_TOP); }
