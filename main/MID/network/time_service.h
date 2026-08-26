#pragma once
#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

esp_err_t time_service_init(void);
void time_service_poll(void);
void time_service_notify_network(void);
void time_service_notify_internet_online(void);
void time_service_apply_http_time(time_t utc_time);
bool time_service_is_initial_location_settled(void);
