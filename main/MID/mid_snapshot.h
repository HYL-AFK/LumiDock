#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

typedef struct {
    bool valid;
    char name[16];
    uint8_t pd_version;
    uint16_t input_voltage_mv;
    uint16_t output_voltage_mv;
    uint16_t type_c_current_ma;
    uint16_t type_a_current_ma;
    bool buck_on;
    bool port_a_on;
    bool port_c_on;
    uint8_t external_device_state;
    uint32_t error_count;
} charging_snapshot_t;

typedef struct {
    bool online;
    bool parsed_valid;
    int32_t pack_mv;
    int32_t current_ma;
    uint8_t soc_percent;
    uint16_t soh_percent;
    uint8_t charge_mos_state;
    uint8_t discharge_mos_state;
    uint8_t protect_1;
    uint8_t protect_2;
    uint8_t protect_3;
    uint32_t timeout_count;
    uint32_t crc_error_count;
} bms_snapshot_t;

typedef struct {
    bool online;
    bool parsed_valid;
    uint32_t timestamp_ms;
    uint8_t sw_protocol[3];
    uint8_t fault_code;
    uint32_t rx_frames;
    uint32_t parsed_frames;
    uint32_t crc_error_count;
    uint32_t parse_error_count;
    uint32_t timeout_count;
} stc_snapshot_t;

typedef struct {
    bool initialized;
    /* connected means STA has a local IP; internet_online means the probe passed. */
    bool connected;
    bool sta_reconnect_enabled;
    bool internet_online;
    uint8_t internet_probe_failures;
    int rssi;
    char ssid[33];
    char ip[16];
} wifi_snapshot_t;

typedef struct {
    bool provisioned;
    bool connected;
    bool provisioning_active;
} bluetooth_snapshot_t;

typedef struct {
    bool initialized;
    bool activated;
    bool request_failed;
    /* True only when this boot observed a server-issued code become bound. */
    bool restart_required;
    char activation_code[32];
} voice_activation_snapshot_t;

typedef enum {
    VOICE_UI_IDLE = 0,
    VOICE_UI_CONNECTING,
    VOICE_UI_LISTENING,
    VOICE_UI_THINKING,
    VOICE_UI_SPEAKING,
    VOICE_UI_ERROR,
    /* The cloud stayed silent past the response deadline; the session is kept
     * alive and the follow-up window is re-armed while this hint is shown. */
    VOICE_UI_TIMEOUT,
} voice_ui_state_t;

typedef struct {
    bool initialized;
    /* True only after a real WakeNet event starts a cloud voice session. */
    bool active;
    /* True when the XiaoZhi WSS hello handshake completed on this client. */
    bool connected;
    /* True when the most recent WSS startup/transport attempt failed. */
    bool connection_failed;
    voice_ui_state_t state;
} voice_session_snapshot_t;

typedef struct {
    bool alarm_active;
    uint8_t alarm_hour;
    uint8_t alarm_minute;
    bool countdown_active;
    uint32_t countdown_remaining_seconds;
    bool stopwatch_running;
    uint32_t stopwatch_seconds;
    bool ringing;
} timer_snapshot_t;

typedef struct {
    bool valid;
    struct tm local_time;
    char timezone[32];
    char weather_text[24];
    int weather_code;
    float temperature_c;
} time_weather_snapshot_t;

typedef struct {
    time_weather_snapshot_t time_weather;
    wifi_snapshot_t wifi;
    bluetooth_snapshot_t bluetooth;
    voice_activation_snapshot_t voice_activation;
    voice_session_snapshot_t voice_session;
    timer_snapshot_t timer;
    charging_snapshot_t charging;
    bms_snapshot_t bms;
    stc_snapshot_t stc;
    bool startup_complete;
} lumidock_snapshot_t;

esp_err_t mid_snapshot_init(void);
/* Acquire a stable, read-only view. Call mid_snapshot_read_release() from the same task. */
bool mid_snapshot_read_acquire(const lumidock_snapshot_t **out);
void mid_snapshot_read_release(void);
void mid_snapshot_get(lumidock_snapshot_t *out);
void mid_snapshot_set_charging(const charging_snapshot_t *value);
void mid_snapshot_set_bms(const bms_snapshot_t *value);
void mid_snapshot_set_stc(const stc_snapshot_t *value);
void mid_snapshot_set_wifi(const wifi_snapshot_t *value);
void mid_snapshot_set_bluetooth(const bluetooth_snapshot_t *value);
void mid_snapshot_set_voice_activation(const voice_activation_snapshot_t *value);
void mid_snapshot_set_voice_session(const voice_session_snapshot_t *value);
void mid_snapshot_set_timer(const timer_snapshot_t *value);
void mid_snapshot_set_time_weather(const time_weather_snapshot_t *value);
void mid_snapshot_set_startup_complete(bool complete);
