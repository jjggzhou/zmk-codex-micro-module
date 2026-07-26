#pragma once

#include <stdbool.h>

struct device;

/* True only for the exact Devicetree input device owned by codex,analog-stick. */
bool codex_activity_defer_input_device(const struct device *device);

/* Enters pinned ZMK's sole activity timestamp/state/event path. */
int codex_zmk_activity_note(void);
