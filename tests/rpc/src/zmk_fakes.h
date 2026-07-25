#pragma once

#include <stdbool.h>
#include <stdint.h>

void rpc_fake_status_set(int profile, uint8_t layer, uint8_t battery,
                         bool powered);
