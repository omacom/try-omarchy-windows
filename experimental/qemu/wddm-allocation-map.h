/* SPDX-License-Identifier: MIT */
/* Host control only; addresses and permissions never come from the guest. */
#ifndef WDDM_ALLOCATION_MAP_H
#define WDDM_ALLOCATION_MAP_H
#include "wddm-fence-map.h"
#define WDDM_ALLOCATION_HUB_DEFAULT_SLOTS 16
#define WDDM_ALLOCATION_HUB_MAX_SLOTS 1024
#define WDDM_ALLOCATION_SLOT_BYTES (1024ULL * 1024)
#define WDDM_ALLOCATION_SOURCE_LIMIT (1ULL << 47)
typedef struct WddmAllocationCommand {
    bool map;
    uint32_t slot;
    uint64_t source, bytes, generation;
} WddmAllocationCommand;
static inline bool wddm_allocation_slots_valid(uint32_t slots)
{
    return slots >= WDDM_ALLOCATION_HUB_DEFAULT_SLOTS &&
           slots <= WDDM_ALLOCATION_HUB_MAX_SLOTS && !(slots & (slots - 1));
}
static inline bool wddm_allocation_command(const char *input,
                                           WddmAllocationCommand *out)
{
    size_t length = 0;
    uint64_t slot = 0;
    WddmAllocationCommand result = {0};
    if (!input || !out) {
        return false;
    }
    while (length < 128 && input[length]) {
        ++length;
    }
    if (length == 128) {
        return false;
    }
    if (length >= 4 && !memcmp(input, "map:", 4)) {
        result.map = true;
        input += 4;
    } else if (length >= 6 && !memcmp(input, "unmap:", 6)) {
        input += 6;
    } else {
        return false;
    }
    if (!wddm_fence_number(&input, &slot, ':') || slot >= WDDM_ALLOCATION_HUB_MAX_SLOTS) {
        return false;
    }
    if (result.map &&
        (!wddm_fence_number(&input, &result.source, ':') || !result.source || result.source % 4096 ||
         !wddm_fence_number(&input, &result.bytes, ':') || !result.bytes || result.bytes % 4096 ||
         result.bytes > WDDM_ALLOCATION_SLOT_BYTES ||
         result.source > WDDM_ALLOCATION_SOURCE_LIMIT - result.bytes)) {
        return false;
    }
    if (!wddm_fence_number(&input, &result.generation, '\0') || !result.generation) {
        return false;
    }
    result.slot = (uint32_t)slot;
    *out = result;
    return true;
}
/* Always constrain commands to the realized device, including older layouts. */
static inline bool wddm_allocation_command_for_slots(const char *input, uint32_t slots,
                                                     WddmAllocationCommand *out)
{
    WddmAllocationCommand command;
    if (!out || !wddm_allocation_slots_valid(slots) ||
        !wddm_allocation_command(input, &command) || command.slot >= slots) {
        return false;
    }
    *out = command;
    return true;
}
static inline bool wddm_allocation_transition(const WddmAllocationCommand *command,
                                              uint64_t last_generation,
                                              uint64_t slot_generation)
{
    return command->map ? !slot_generation && command->generation > last_generation
                        : slot_generation && command->generation == slot_generation;
}
#endif
