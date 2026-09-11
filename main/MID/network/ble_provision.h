#pragma once

/*
 * BLE provisioning interface based on standard BluFi.
 *
 * The BLE advertising/scan name is "ESPARK-XXXXXX", where XXXXXX
 * is the last six hexadecimal digits of the Bluetooth MAC address.
 * Wi-Fi credentials are provisioned through the standard BluFi profile,
 * not the old custom FFF0/FFF1/FFF2 command protocol.
 */

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "app_config.h"
#include "wifi_manager.h"

typedef enum {
    BLE_WINDOW_PROVISION = 0, /* First-time no-WiFi provisioning; deep-sleep on timeout. */
    BLE_WINDOW_BOOT      = 1, /* Boot-time re-provision window; resume networking on timeout. */
} ble_window_kind_t;

void ble_provision_init(const app_config_t *cfg, bool open_window, ble_window_kind_t kind);
esp_err_t ble_provision_open_window(void);
void ble_provision_close_window(void);
void ble_provision_handle_network_event(network_event_type_t event);
bool ble_provision_is_window_active(void);

/* BLE runtime state for UI/status queries. */
bool ble_provision_is_ready(void);
bool ble_provision_is_advertising(void);
bool ble_provision_is_connected(void);
bool ble_provision_is_notify_enabled(void);

/* Current BluFi connection handle; usually 0xFFFF when not connected. */
uint16_t ble_provision_get_conn_handle(void);

/* Current advertising name, derived from the Bluetooth MAC address. */
const char *ble_provision_get_adv_name(void);
