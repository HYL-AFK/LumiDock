#pragma once

#include <stdint.h>

#include "esp_err.h"

/* On-board single WS2812B RGB status LED on GPIO48.
 * RMT-driven, independent of the GPIO13/14 strip driver. */
esp_err_t onboard_led_driver_init(void);
esp_err_t onboard_led_driver_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
void onboard_led_driver_clear(void);
