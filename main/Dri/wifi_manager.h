#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Wi-Fi 管理接口。
 *
 * 本模块负责 ESP32-S3 的 Wi-Fi APSTA 模式：
 * - STA：连接手机通过 BLE 下发的路由器/热点。
 * - AP：设备自己的兜底热点，便于未入网或调试时直连。
 *
 * 上层只调用这里的接口，不直接调用 esp_wifi_set_config/esp_wifi_connect。
 */

#include "esp_netif_ip_addr.h"
#include "esp_netif_types.h"
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "app_config.h"

/* Wi-Fi 状态变化回调。BLE 广播状态位刷新就是通过这个回调触发的。 */
typedef void (*wifi_status_cb_t)(void);

typedef enum {
    NETWORK_EVENT_STA_START = 0,
    NETWORK_EVENT_STA_CONNECTED,
    NETWORK_EVENT_STA_DISCONNECTED,
    NETWORK_EVENT_STA_GOT_IP,
    NETWORK_EVENT_STA_LOST_IP,
    NETWORK_EVENT_AP_START,
    NETWORK_EVENT_AP_STOP,
    NETWORK_EVENT_SCAN_DONE,
    NETWORK_EVENT_SERVICES_READY,
    NETWORK_EVENT_OPEN_BLUFI,
    NETWORK_EVENT_CLOSE_BLUFI,
    NETWORK_EVENT_STATUS_REFRESH,
    NETWORK_EVENT_BLUFI_DISCONNECT,
    NETWORK_EVENT_BLUFI_STATUS,
    NETWORK_EVENT_BLUFI_SCAN,
    NETWORK_EVENT_BLUFI_CONNECT,
} network_event_type_t;

typedef struct {
    network_event_type_t type;
    union {
        wifi_event_sta_disconnected_t disconnected;
        wifi_event_sta_connected_t connected;
        ip_event_got_ip_t got_ip;
    } data;
} network_coordinator_event_t;

esp_err_t wifi_manager_init(const app_config_t *cfg);
bool wifi_manager_post_network_event(const network_coordinator_event_t *event);

/* 从 cfg 中选择可用且信号最强的已保存 Wi-Fi，并发起 STA 连接。 */
void wifi_manager_connect_sta(const app_config_t *cfg);

/* Connect directly to the specified STA target, used by BluFi provisioning. */
void wifi_manager_connect_specific_sta(const app_config_t *cfg,
                                       const char *ssid,
                                       const char *pass,
                                       const uint8_t *bssid,
                                       bool bssid_set);

/* 断开 STA，并清空 ESP-IDF Wi-Fi 驱动里的 STA 配置，避免清除 NVS 后又自动连回去。 */
void wifi_manager_disconnect_sta(void);
void wifi_manager_reconnect_sta(void);

void wifi_manager_enable_sta_reconnect(bool enable);

/* 注册统一状态回调，供 BLE 广播/UI/云端服务联动刷新。 */
void wifi_manager_set_status_cb(wifi_status_cb_t cb);

/* 动态修改 SoftAP 名称和密码。密码长度小于 8 时不会被接受。 */
void wifi_manager_set_ap_name(const char *ap_name);
void wifi_manager_set_ap_password(const char *ap_pass);

/* 状态查询接口，主要给 BLE 广播状态位、App 查询和日志使用。 */
/* STA has an association and a DHCP-assigned local IP. */
bool wifi_manager_is_sta_connected(void);
bool wifi_manager_is_sta_reconnect_enabled(void);
/* The background HTTPS probe can reach the public Internet. */
bool wifi_manager_is_internet_online(void);
uint8_t wifi_manager_get_internet_probe_failures(void);
bool wifi_manager_is_ap_started(void);
uint32_t wifi_manager_get_sta_failure_count(void);

/* 获取当前已连接 STA 的运行时信息。 */
int wifi_manager_get_sta_rssi(void);
int wifi_manager_get_sta_channel(void);
const char *wifi_manager_get_sta_phy_string(void);
int wifi_manager_get_ap_channel(void);

/* 获取 STA/AP IP 信息。 */
bool wifi_manager_get_sta_ip(esp_ip4_addr_t *ip);
bool wifi_manager_get_ap_ip(esp_ip4_addr_t *ip);

/* 获取 STA/AP MAC 信息。 */
bool wifi_manager_get_sta_mac(uint8_t mac[6]);
bool wifi_manager_get_ap_mac(uint8_t mac[6]);
bool wifi_manager_get_sta_bssid(uint8_t mac[6]);

/* 获取当前缓存的字符串状态，便于 UI 和日志直接使用。 */
const char *wifi_manager_get_sta_ssid(void);
const char *wifi_manager_get_ap_name(void);
const char *wifi_manager_get_sta_ip_str(void);
const char *wifi_manager_get_ap_ip_str(void);
