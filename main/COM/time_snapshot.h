#pragma once
#include <stdbool.h>
#include <time.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { struct tm local_time; time_t unix_time; bool from_rtc; bool from_sntp; uint32_t revision; } time_snapshot_t;
esp_err_t time_snapshot_init(void);
bool time_snapshot_is_ready(void);
void time_snapshot_get(time_snapshot_t *out);
void time_snapshot_set(const time_snapshot_t *value);
