#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t status_led_service_init(void);
void status_led_service_set_critical_fault(bool active);
/* A user command holds the complete strip off until this is set true or the
 * device restarts. The status task remains the only WS2812 writer. */
esp_err_t status_led_service_set_user_enabled(bool enabled);
/* Coordinator-owned overlay for the complete UI boot animation. */
void status_led_service_set_boot_animation_active(bool active);
