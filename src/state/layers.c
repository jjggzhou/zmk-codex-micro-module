#include <errno.h>

#include <zmk/keymap.h>

#include "state_internal.h"

#define RECONCILE_ATTEMPTS 2U

static bool layer_state_matches(const zmk_keymap_layer_id_t ids[CODEX_LAYER_COUNT],
                                const bool wanted[CODEX_LAYER_COUNT])
{
    for (uint8_t index = 1U; index < CODEX_LAYER_COUNT; index++) {
        if (zmk_keymap_layer_active(ids[index]) != wanted[index]) {
            return false;
        }
    }
    return true;
}

static int reconcile_layer_state(const zmk_keymap_layer_id_t ids[CODEX_LAYER_COUNT],
                                 const bool wanted[CODEX_LAYER_COUNT])
{
    int first_error = 0;

    for (uint8_t attempt = 0U; attempt < RECONCILE_ATTEMPTS; attempt++) {
        bool unwanted_active = false;

        /* Clear old overlays before enabling the target, preserving exclusivity. */
        for (uint8_t index = 1U; index < CODEX_LAYER_COUNT; index++) {
            int err;

            if (wanted[index] || !zmk_keymap_layer_active(ids[index])) {
                continue;
            }
            err = zmk_keymap_layer_deactivate(ids[index]);
            if (first_error == 0 && err != 0) {
                first_error = err;
            }
        }
        for (uint8_t index = 1U; index < CODEX_LAYER_COUNT; index++) {
            if (!wanted[index] && zmk_keymap_layer_active(ids[index])) {
                unwanted_active = true;
                break;
            }
        }
        if (unwanted_active) {
            continue;
        }
        for (uint8_t index = 1U; index < CODEX_LAYER_COUNT; index++) {
            int err;

            if (!wanted[index] || zmk_keymap_layer_active(ids[index])) {
                continue;
            }
            err = zmk_keymap_layer_activate(ids[index]);
            if (first_error == 0 && err != 0) {
                first_error = err;
            }
        }
        if (layer_state_matches(ids, wanted)) {
            break;
        }
    }
    return first_error;
}

uint8_t codex_layers_current_indicator(void)
{
    return codex_layer_indicator_bits(zmk_keymap_highest_layer_active());
}

int codex_layers_cycle(void)
{
    uint8_t current = zmk_keymap_highest_layer_active();
    uint8_t next = current < CODEX_LAYER_COUNT - 1U ? current + 1U : 0U;
    zmk_keymap_layer_id_t ids[CODEX_LAYER_COUNT];
    bool original[CODEX_LAYER_COUNT] = {false};
    bool wanted[CODEX_LAYER_COUNT] = {false};
    int first_error;

    for (uint8_t index = 0U; index < CODEX_LAYER_COUNT; index++) {
        ids[index] = zmk_keymap_layer_index_to_id(index);
        if (ids[index] == ZMK_KEYMAP_LAYER_ID_INVAL) {
            codex_indicators_render(codex_layers_current_indicator());
            return -EINVAL;
        }
        for (uint8_t prior = 0U; prior < index; prior++) {
            if (ids[index] == ids[prior]) {
                codex_indicators_render(codex_layers_current_indicator());
                return -EINVAL;
            }
        }
        original[index] = zmk_keymap_layer_active(ids[index]);
    }
    if (next != 0U) {
        wanted[next] = true;
    }

    first_error = reconcile_layer_state(ids, wanted);
    if (!layer_state_matches(ids, wanted)) {
        int rollback_error = reconcile_layer_state(ids, original);

        if (first_error == 0) {
            first_error = rollback_error != 0 ? rollback_error : -EIO;
        }
    }

    codex_indicators_render(codex_layers_current_indicator());
    return first_error;
}
