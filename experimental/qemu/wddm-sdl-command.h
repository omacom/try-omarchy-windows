/* SPDX-License-Identifier: MIT */
/* Host-only relay grammar. No process handles or pointers cross this boundary. */
#ifndef WDDM_SDL_COMMAND_H
#define WDDM_SDL_COMMAND_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define WDDM_SDL_MAX_COUNTER ((UINT64_C(1) << 48) - 1)
typedef enum WddmSdlAction {
    WDDM_SDL_ATTACH, WDDM_SDL_PRESENT, WDDM_SDL_AUDIT, WDDM_SDL_DETACH
} WddmSdlAction;
typedef struct WddmSdlCommand {
    WddmSdlAction action;
    uint64_t generation, frame;
} WddmSdlCommand;

static inline bool wddm_sdl_number(const char **cursor, uint64_t *number)
{
    const char *p = *cursor;
    uint64_t value = 0;
    if (*p < '1' || *p > '9') {
        return false;
    }
    do {
        unsigned digit = *p++ - '0';
        if (value > (WDDM_SDL_MAX_COUNTER - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *number = value;
    return true;
}

static inline bool wddm_sdl_parse(const char *text, WddmSdlCommand *out)
{
    const char *p;
    WddmSdlCommand value = {0};
    if (!text || strlen(text) > 96) {
        return false;
    }
    if (!strncmp(text, "attach:", 7)) {
        value.action = WDDM_SDL_ATTACH;
        p = text + 7;
    } else if (!strncmp(text, "present:", 8)) {
        value.action = WDDM_SDL_PRESENT;
        p = text + 8;
    } else if (!strncmp(text, "audit:", 6)) {
        value.action = WDDM_SDL_AUDIT;
        p = text + 6;
    } else if (!strncmp(text, "detach:", 7)) {
        value.action = WDDM_SDL_DETACH;
        p = text + 7;
    } else {
        return false;
    }
    if (!wddm_sdl_number(&p, &value.generation)) {
        return false;
    }
    if (value.action == WDDM_SDL_PRESENT || value.action == WDDM_SDL_AUDIT) {
        if (*p++ != ':' || !wddm_sdl_number(&p, &value.frame)) {
            return false;
        }
    }
    if (*p) {
        return false;
    }
    *out = value;
    return true;
}

static inline bool wddm_sdl_hex(const char *value, unsigned bytes)
{
    if (!value || strlen(value) != bytes * 2) {
        return false;
    }
    for (unsigned i = 0; i < bytes * 2; ++i) {
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}
#endif
