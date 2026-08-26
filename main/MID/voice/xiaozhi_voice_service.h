#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t xiaozhi_voice_service_init(void);
bool xiaozhi_voice_service_is_ready(void);
/* Request exactly one cloud greeting through the verified listen/detect path.
 * The state machine owns all actual protocol and AFE transitions. */
esp_err_t xiaozhi_voice_service_start_boot_greeting(void);
void xiaozhi_voice_service_network_lost(void);
