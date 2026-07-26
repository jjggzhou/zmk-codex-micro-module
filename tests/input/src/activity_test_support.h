#pragma once

#include <stddef.h>

void codex_activity_test_reset_events(void);
size_t codex_activity_test_event_count(void);
void codex_activity_test_fail_next_notes(size_t count);
size_t codex_activity_test_note_call_count(void);
