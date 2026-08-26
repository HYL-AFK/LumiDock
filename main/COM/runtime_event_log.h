#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    RUNTIME_EVENT_BOOT = 1,
    RUNTIME_EVENT_WIFI_CONNECTED,
    RUNTIME_EVENT_WIFI_FAILURE,
    RUNTIME_EVENT_SNTP_SYNC,
    RUNTIME_EVENT_WEATHER_SUCCESS,
    RUNTIME_EVENT_WEATHER_FAILURE,
    RUNTIME_EVENT_BRIGHTNESS_CHANGE,
    RUNTIME_EVENT_DISPLAY_ERROR,
    RUNTIME_EVENT_INTERNET_PROBE,
    RUNTIME_EVENT_LOCATION_SUCCESS,
    RUNTIME_EVENT_LOCATION_FAILURE,
    RUNTIME_EVENT_BLE_WINDOW_OPEN,
    RUNTIME_EVENT_BLE_TIMEOUT,
    RUNTIME_EVENT_BLE_PROVISION_ACCEPTED,
    RUNTIME_EVENT_WATCHDOG_FAILURE,
} runtime_event_type_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t event_type;
    uint64_t timestamp;
    int32_t value1;
    int32_t value2;
    uint32_t flags;
    uint32_t sequence;
    uint32_t record_crc32;
} runtime_event_record_t;

esp_err_t runtime_event_log_init(void);
esp_err_t runtime_event_log_append(runtime_event_type_t type,
                                   int32_t value1,
                                   int32_t value2,
                                   uint32_t flags);
esp_err_t runtime_event_log_get(uint16_t offset_from_oldest,
                                runtime_event_record_t *out);
uint16_t runtime_event_log_count(void);
esp_err_t runtime_event_log_clear(void);
void runtime_event_log_dump_recent(uint16_t max_records);
