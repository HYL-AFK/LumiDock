#pragma once

#include "esp_err.h"

/* Owns boot-only coordination between UI phase, local sound, LED overlay,
 * and the once-per-power-cycle XiaoZhi greeting. */
esp_err_t startup_coordinator_init(void);
