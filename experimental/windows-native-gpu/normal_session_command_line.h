// SPDX-License-Identifier: MIT
#pragma once
#include <string.h>

static inline int wddm_normal_session_selected(char* text) {
    if(!text) return 0;
    unsigned selected=0;
    char* save=NULL;
    for(char* token=strtok_r(text," \t\r\n",&save);token;token=strtok_r(NULL," \t\r\n",&save)) {
        if(!strncmp(token,"tryomarchy.nvgpu=",sizeof("tryomarchy.nvgpu=")-1)) {
            if(strcmp(token,"tryomarchy.nvgpu=1") || ++selected!=1) return 0;
        }
    }
    return selected==1;
}
