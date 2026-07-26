#include <errno.h>

#include <zmk/keymap.h>

#include "state_internal.h"

uint8_t codex_layers_current_indicator(void)
{
    return codex_layer_indicator_bits(zmk_keymap_highest_layer_active());
}

int codex_layers_cycle(void)
{
    uint8_t current = zmk_keymap_highest_layer_active();
    uint8_t next = current < CODEX_LAYER_COUNT - 1U ? current + 1U : 0U;
    int first_error = 0;

    if (next != 0U) {
        zmk_keymap_layer_id_t next_id = zmk_keymap_layer_index_to_id(next);

        if (next_id == ZMK_KEYMAP_LAYER_ID_INVAL) {
            codex_indicators_render(codex_layers_current_indicator());
            return -EINVAL;
        }
        first_error = zmk_keymap_layer_activate(next_id);
        if (first_error != 0) {
            codex_indicators_render(codex_layers_current_indicator());
            return first_error;
        }
    }

    for (uint8_t layer = 1U; layer < CODEX_LAYER_COUNT; layer++) {
        zmk_keymap_layer_id_t layer_id;
        int err;

        if (layer == next) {
            continue;
        }
        layer_id = zmk_keymap_layer_index_to_id(layer);
        err = layer_id == ZMK_KEYMAP_LAYER_ID_INVAL
                  ? -EINVAL
                  : zmk_keymap_layer_deactivate(layer_id);
        if (first_error == 0 && err != 0) {
            first_error = err;
        }
    }

    codex_indicators_render(codex_layers_current_indicator());
    return first_error;
}
