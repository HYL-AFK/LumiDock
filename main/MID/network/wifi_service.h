#pragma once

#include "esp_err.h"

esp_err_t wifi_service_init(void);
/* Call after time/weather consumers have been initialized. */
void wifi_service_notify_services_ready(void);
void wifi_service_open_provision_window(void);
void wifi_service_poll(void);
