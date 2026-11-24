// SPDX-License-Identifier: MIT
// Classify the pinned Hyprland CLI without rewriting or consuming its argv.
#pragma once
#include <string.h>

static inline int wddm_hyprland_renders(int argc,char* const argv[]) {
    if(argc<1 || !argv || !argv[0]) return 0;
    for(int index=1;index<argc;++index) {
        const char* arg=argv[index];
        if(!arg) return 0;
        if(!strcmp(arg,"--help") || !strcmp(arg,"-h") || !strcmp(arg,"--version") ||
           !strcmp(arg,"-v") || !strcmp(arg,"--version-json") || !strcmp(arg,"--systeminfo") ||
           !strcmp(arg,"--verify-config")) return 0;
        if(!strcmp(arg,"--config") || !strcmp(arg,"-c") || !strcmp(arg,"--socket") ||
           !strcmp(arg,"--wayland-fd") || !strcmp(arg,"--watchdog-fd") || !strcmp(arg,"--locked-cmd")) {
            if(++index>=argc || !argv[index]) return 0;
            continue; // A query-looking value is still a value, not an option.
        }
        if(strcmp(arg,"--safe-mode") && strcmp(arg,"--locked") && strcmp(arg,"--i-am-really-stupid"))
            return 0; // Let Hyprland itself report unsupported syntax.
    }
    return 1;
}
