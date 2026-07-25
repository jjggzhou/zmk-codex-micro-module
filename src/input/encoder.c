#include <codex/input.h>

#include <errno.h>

#include "input_internal.h"

int codex_input_encoder_press(bool pressed)
{
    return codex_input_enqueue_event(CODEX_INPUT_EVENT_ENC_CLK, pressed);
}

int codex_input_encoder_tick(int direction)
{
    if (direction == 1) {
        /* A hardware detent is one momentary notification, not a key pair. */
        return codex_input_enqueue_event(CODEX_INPUT_EVENT_ENC_CW, true);
    }
    if (direction == -1) {
        return codex_input_enqueue_event(CODEX_INPUT_EVENT_ENC_CC, true);
    }
    return -EINVAL;
}
