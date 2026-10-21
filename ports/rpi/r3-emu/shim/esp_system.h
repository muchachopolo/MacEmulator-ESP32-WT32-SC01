/* Pi shim: esp_restart() reboots the Pi (the emulated Mac restarts from power-on). */
#pragma once
#include "pi_platform.h"
static inline void esp_restart(void) { pi_restart(); }
