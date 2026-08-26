#pragma once

#include "esp_err.h"

typedef void (*button_long_press_cb_t)(void);
typedef void (*button_short_press_cb_t)(void);

esp_err_t button_driver_init(void);
esp_err_t button_driver_set_long_press_callback(button_long_press_cb_t callback);
esp_err_t button_driver_set_short_press_callback(button_short_press_cb_t callback);
