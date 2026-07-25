#pragma once

#include <stdbool.h>
#include <stdint.h>

int codex_input_key(const char *id, bool pressed, uint8_t agent_index);
int codex_input_encoder_press(bool pressed);
int codex_input_encoder_tick(int direction);

#if defined(CONFIG_ZTEST)
void codex_input_test_reset(void);
#endif
