#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t weather_service_init(void);
void weather_service_poll(void);
void weather_service_notify_network(void);
bool weather_service_is_initial_request_settled(void);
