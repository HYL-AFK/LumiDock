#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t ws2812_driver_init(void);
esp_err_t ws2812_driver_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t ws2812_driver_set_all(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t ws2812_driver_set_pixel(uint8_t index, uint8_t red, uint8_t green, uint8_t blue);
void ws2812_driver_clear(void);
esp_err_t ws2812_driver_show(void);
