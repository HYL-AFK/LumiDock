#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Registers this device with the XiaoZhi OTA endpoint from the supplied
 * reference project. A returned activation code means the user must bind the
 * device in the XiaoZhi web console. */
esp_err_t xiaozhi_activation_service_init(void);
void xiaozhi_activation_service_notify_network(void);
void xiaozhi_activation_service_network_lost(void);
/* Drop an in-RAM WSS URL/token and fetch a new pair on the next OTA request.
 * This never clears a user binding or the persistent Client-Id. */
void xiaozhi_activation_service_refresh_credentials(void);

bool xiaozhi_activation_service_is_activated(void);
const char *xiaozhi_activation_service_get_websocket_url(void);
const char *xiaozhi_activation_service_get_websocket_token(void);
const char *xiaozhi_activation_service_get_client_id(void);
const char *xiaozhi_activation_service_get_device_id(void);
