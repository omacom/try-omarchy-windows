// SPDX-License-Identifier: MIT
// CPU-only substitute for guest device discovery; all service logic is real.
#define _GNU_SOURCE
#include <string.h>
static int cpu_devices(char port[128],char render[64]) {
    strcpy(port,"/dev/cpu-mock-port");strcpy(render,"/dev/dri/cpu-mock-render");return 0;
}
#define WDDM_SESSION_PREPARE_DEVICES cpu_devices
#include "normal_session_service.c"
