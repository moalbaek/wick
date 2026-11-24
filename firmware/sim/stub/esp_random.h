#pragma once
#include <stdint.h>
// The renderer sets the next value, so it can pick the kids-mode scene it wants to draw.
extern uint32_t sim_random_next;
static inline uint32_t esp_random(void) { return sim_random_next; }
