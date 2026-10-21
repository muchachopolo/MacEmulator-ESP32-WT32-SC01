/* Host shim: esp_restart() ends the run (on the ESP32 it cold-boots the
 * board, i.e. the emulated Mac restarts from power-on). */
#pragma once
#include <stdio.h>
#include <stdlib.h>
static inline void esp_restart(void)
{
    fprintf(stderr, "H host: esp_restart() -> exit (board would cold-boot)\n");
    exit(0);
}
