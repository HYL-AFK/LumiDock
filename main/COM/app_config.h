#pragma once

/*
 * Shared application configuration definitions.
 *
 * This module only defines the in-memory config structure and the NVS
 * persistence interface. Wi-Fi, BLE, and other business modules should
 * read/write config through app_config.c.
 */

#include <stdbool.h>
#include "esp_err.h"

#define WIFI_SSID_MAX_LEN      32
#define WIFI_PASS_MAX_LEN      64
#define BLE_NAME_MAX_LEN       32
#define AP_NAME_MAX_LEN        32
#define AP_PASS_MAX_LEN        64
#define WIFI_PROFILE_MAX_NUM    5

typedef struct {
    /* One saved STA Wi-Fi profile. */
    char wifi_ssid[WIFI_SSID_MAX_LEN + 1];
    char wifi_pass[WIFI_PASS_MAX_LEN + 1];
    bool valid;
} wifi_profile_t;

typedef struct {
    /* Saved STA Wi-Fi profiles; wifi_manager prefers the strongest matching SSID. */
    wifi_profile_t wifi_profiles[WIFI_PROFILE_MAX_NUM];
    int wifi_count;
    char last_wifi_ssid[WIFI_SSID_MAX_LEN + 1];
    bool has_last_wifi_ssid;

    /* User display name. BLE advertising name stays fixed as ESPARK-XXXXXX. */
    char ble_name[BLE_NAME_MAX_LEN + 1];

    /* SoftAP name and password. Passwords shorter than 8 are treated as open AP by ESP-IDF. */
    char ap_name[AP_NAME_MAX_LEN + 1];
    char ap_pass[AP_PASS_MAX_LEN + 1];

    /* Distinguish user-configured values from defaults. */
    bool has_wifi;
    bool has_ble_name;
    bool has_ap_name;
    bool has_ap_pass;
} app_config_t;

/* Load application config from NVS, or fill defaults if nothing valid exists. */
void app_config_load(app_config_t *cfg);

/* Save current config to NVS. */
esp_err_t app_config_save(const app_config_t *cfg);
esp_err_t app_config_save_last_wifi_ssid(const char *ssid);

/* Fill a default config in memory without touching NVS. */
void app_config_set_default(app_config_t *cfg);

/* Add or update one STA Wi-Fi profile; same SSID updates the saved password. */
bool app_config_add_or_update_wifi(app_config_t *cfg, const char *ssid, const char *pass);

/* Erase the whole application config namespace. */
esp_err_t app_config_erase_all(void);

/* Erase only STA Wi-Fi profiles; BLE name and AP config stay untouched. */
esp_err_t app_config_erase_wifi(void);
