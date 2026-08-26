#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define LOCATION_TIMEZONE_MAX_LEN 64

typedef struct {
    double latitude;
    double longitude;
    char timezone[LOCATION_TIMEZONE_MAX_LEN];
    bool valid;
    uint32_t revision;
} location_snapshot_t;

esp_err_t location_snapshot_init(void);
bool location_snapshot_get(location_snapshot_t *out);
esp_err_t location_snapshot_set(double latitude, double longitude,
                                const char *timezone);
