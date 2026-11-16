/* SPDX-License-Identifier: MIT */
/* Host control messages only. This is not a guest PCI or virtio protocol. */
#ifndef WDDM_FENCE_MAP_H
#define WDDM_FENCE_MAP_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define WDDM_FENCE_HUB_PAGES 64
#define WDDM_FENCE_HUB_MAX_PAGES 512
static inline bool wddm_fence_slots_valid(uint32_t slots)
{
    return slots == WDDM_FENCE_HUB_PAGES || slots == 128 || slots == 256 || slots == WDDM_FENCE_HUB_MAX_PAGES;
}
typedef struct WddmFenceCommand {
    bool map;
    uint32_t slot;
    uint64_t source, generation;
} WddmFenceCommand;

static inline bool wddm_fence_number(const char **input, uint64_t *number,
                                     char delimiter)
{
    const char *p = *input;
    uint64_t result = 0;
    if (*p < '0' || *p > '9') {
        return false;
    }
    do {
        const unsigned digit = *p++ - '0';
        if (result > (UINT64_MAX - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    if (*p != delimiter) {
        return false;
    }
    *number = result;
    *input = p + (delimiter ? 1 : 0);
    return true;
}

static inline bool wddm_fence_command(const char *input, WddmFenceCommand *out)
{
    size_t length = 0;
    uint64_t slot = 0;
    WddmFenceCommand result = {0};
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
    if (!wddm_fence_number(&input, &slot, ':') || slot >= WDDM_FENCE_HUB_MAX_PAGES) {
        return false;
    }
    if (result.map && (!wddm_fence_number(&input, &result.source, ':') ||
                       !result.source || result.source % 8 ||
                       (result.source & 4095) > 4088)) {
        return false;
    }
    if (!wddm_fence_number(&input, &result.generation, '\0') || !result.generation) {
        return false;
    }
    result.slot = (uint32_t)slot;
    *out = result;
    return true;
}

static inline bool wddm_fence_command_for_slots(const char *input, uint32_t slots, WddmFenceCommand *out)
{
    return wddm_fence_slots_valid(slots) && wddm_fence_command(input, out) && out->slot < slots;
}

static inline bool wddm_fence_transition(const WddmFenceCommand *command,
                                        uint64_t last_generation,
                                        uint64_t slot_generation)
{
    return command->map ? !slot_generation && command->generation > last_generation
                        : slot_generation && command->generation == slot_generation;
}
#endif
