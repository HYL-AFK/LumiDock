#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef enum {
    WEATHER_TEMP_SOURCE_NONE = 0,
    WEATHER_TEMP_SOURCE_NETWORK,
} weather_temperature_source_t;

typedef struct {
    float temperature_c;
    int weather_code;
    bool valid;
    bool temperature_valid;
    weather_temperature_source_t temperature_source;
    time_t updated_at;
    /* 充电策略的输入：明天气温直接决定续航（低温掉电快），降水概率决定要不要
     * 提前把电补满。单独给一个 forecast_valid，缺预报时不影响既有的温度/天气
     * 显示链路——那两条走的是 valid / temperature_valid。 */
    bool forecast_valid;
    int8_t today_temp_min_c;
    int8_t today_temp_max_c;
    uint8_t today_precip_prob;
    int8_t tomorrow_temp_min_c;
    int8_t tomorrow_temp_max_c;
    uint8_t tomorrow_precip_prob;
} weather_snapshot_t;

esp_err_t weather_snapshot_init(void);
void weather_snapshot_get(weather_snapshot_t *out);
void weather_snapshot_set(const weather_snapshot_t *value);
