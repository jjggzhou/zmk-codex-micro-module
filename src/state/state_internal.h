#pragma once

#include <stdint.h>

#include <codex/state.h>

int codex_layers_cycle(void);
uint8_t codex_layers_current_indicator(void);

enum codex_connection_choice codex_connection_initial_choice(void);
int codex_connection_apply_choice(enum codex_connection_choice requested,
                                  enum codex_connection_choice *actual);
int codex_connection_clear_choice(enum codex_connection_choice choice);

void codex_indicators_render(uint8_t bits);
void codex_indicators_reset(void);
