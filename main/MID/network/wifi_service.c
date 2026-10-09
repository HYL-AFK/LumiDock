#include "wifi_service.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "wifi_manager.h"
#include "ble_provision.h"
#include "mid_snapshot.h"
#include "news_service.h"
#include "tariff_service.h"
#include "time_service.h"
#include "xiaozhi_activation_service.h"
#include "xiaozhi_voice_service.h"

static const char *TAG = "wifi_service";
static app_config_t s_config;
static bool s_network_services_notified;
static bool s_network_services_ready;
static bool s_time_service_notified;

static void update_snapshot(void)
{
    wifi_snapshot_t wifi = {0};
    wifi.initialized = true;
    wifi.connected = wifi_manager_is_sta_connected();
    wifi.sta_reconnect_enabled = wifi_manager_is_sta_reconnect_enabled();
    wifi.internet_online = wifi_manager_is_internet_online();
    wifi.internet_probe_failures = wifi_manager_get_internet_probe_failures();
    wifi.rssi = wifi_manager_get_sta_rssi();
    (void)strlcpy(wifi.ssid, wifi_manager_get_sta_ssid(), sizeof(wifi.ssid));
    (void)strlcpy(wifi.ip, wifi_manager_get_sta_ip_str(), sizeof(wifi.ip));
    mid_snapshot_set_wifi(&wifi);

    bluetooth_snapshot_t bluetooth = {
        .provisioned = ble_provision_is_ready(),
        .connected = ble_provision_is_connected(),
        .provisioning_active = ble_provision_is_window_active(),
    };
    mid_snapshot_set_bluetooth(&bluetooth);
}

static void wifi_status_changed(void)
{
    update_snapshot();
    const bool sta_online = wifi_manager_is_sta_connected();
    const bool internet_online = wifi_manager_is_internet_online();
    if (!sta_online) {
        s_network_services_notified = false;
        s_time_service_notified = false;
        xiaozhi_activation_service_network_lost();
        xiaozhi_voice_service_network_lost();
        if (wifi_manager_get_sta_failure_count() >= 3U &&
            !ble_provision_is_window_active() &&
            !ble_provision_is_connected()) {
            (void)ble_provision_open_window();
        }
    } else if (s_network_services_ready) {
        /* SNTP does not need the public probe. Start it as soon as DHCP gives
         * us an IP; location/weather/cloud services remain probe-gated. */
        if (!s_time_service_notified) {
            time_service_notify_network();
            s_time_service_notified = true;
        }
    }
    if (sta_online && internet_online && s_network_services_ready &&
        !s_network_services_notified) {
        /* The location task must be released after the online state is visible,
         * otherwise the HTTP Date event can be consumed while it still reads offline. */
        time_service_notify_internet_online();
        xiaozhi_activation_service_notify_network();
        /* 电价是充电策略的输入之一，同样要等公网探测通过才值得去拉。
           断网分支会把 s_network_services_notified 清掉，所以重连后会再通知一次。 */
        tariff_service_notify_network();
        /* 新闻同理：开机时四个源连着拉要好几秒，只有公网确实通了才值得动手。 */
        news_service_notify_network();
        s_network_services_notified = true;
        ble_provision_close_window();
    }
}


esp_err_t wifi_service_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    app_config_load(&s_config);
    wifi_manager_set_status_cb(wifi_status_changed);
    err = wifi_manager_init(&s_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi manager init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (s_config.has_wifi && s_config.wifi_count > 0) {
        ESP_LOGI(TAG, "saved Wi-Fi profile found; opening boot re-provision window, connecting STA");
        ble_provision_init(&s_config, true, BLE_WINDOW_BOOT);
        wifi_manager_connect_sta(&s_config);
    } else {
        ESP_LOGI(TAG, "no saved Wi-Fi profile; starting BluFi provisioning");
        ble_provision_init(&s_config, true, BLE_WINDOW_PROVISION);
    }
    update_snapshot();
    ESP_LOGI(TAG, "APSTA Wi-Fi state machine started");
    return ESP_OK;
}

void wifi_service_poll(void)
{
    update_snapshot();
}

void wifi_service_notify_services_ready(void)
{
    s_network_services_ready = true;
    network_coordinator_event_t event = {.type = NETWORK_EVENT_SERVICES_READY};
    (void)wifi_manager_post_network_event(&event);
}

void wifi_service_open_provision_window(void)
{
    network_coordinator_event_t event = {.type = NETWORK_EVENT_OPEN_BLUFI};
    (void)wifi_manager_post_network_event(&event);
}
