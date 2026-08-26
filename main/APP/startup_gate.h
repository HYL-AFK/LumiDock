#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Configure the board button wake source and put the device into startup sleep. */
esp_err_t startup_gate_init(void);

/* Return true only after the board button has been held for the startup interval. */
bool startup_gate_wait_for_long_press(void);

/* Shared sleep entry for failed BLE provisioning and other shutdown paths. */
void startup_gate_enter_deep_sleep(void);
