#pragma once

#include <stdbool.h>
#include <stddef.h>

void codex_input_test_reset_capture(void);
void codex_input_test_wait_for_events(size_t count);
const char *codex_input_test_event(size_t index);
size_t codex_input_test_event_count(void);
void codex_input_test_set_router_result(int result);
void codex_input_test_set_block_router(bool block);
int codex_input_test_wait_router_entered(void);
void codex_input_test_release_router(void);
