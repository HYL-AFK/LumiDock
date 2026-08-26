#pragma once

#include "esp_err.h"
#include "mid_snapshot.h"

esp_err_t stc_service_init(void);
void stc_service_poll(void);
bool stc_service_get_snapshot(stc_snapshot_t *out);
