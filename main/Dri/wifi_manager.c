#include "wifi_manager.h"

#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_log.h"
#include "runtime_event_log.h"
#include "internet_health_policy.h"
#include "watchdog_service.h"
#include "http_memory_log.h"
#include "time_service.h"
#include "ble_provision.h"

/*
 * Wi-Fi APSTA 管理模块。
 *
 * 启动后固定工作在 WIFI_MODE_APSTA：
 * - AP 侧：始终提供设备热点，默认 ESP32S3-SETUP / 12345678。
 * - STA 侧：根据 NVS 中保存的 Wi-Fi 列表，扫描环境并连接信号最强的可用 SSID。
 *
 * BLE 配网模块收到新配置后，只需要更新 app_config，再调用 wifi_manager_connect_sta()。
 */

static const char *TAG = "wifi_manager";

static void internet_probe_delay(uint32_t delay_ms)
{
    TickType_t remaining = pdMS_TO_TICKS(delay_ms);
    const TickType_t slice = pdMS_TO_TICKS(500);
    while (remaining > 0) {
        const TickType_t current = remaining > slice ? slice : remaining;
        vTaskDelay(current);
        watchdog_service_feed();
        remaining -= current;
    }
}

/*
 * 当前仍然采用“扫描保存列表并选择最强 SSID”的策略。
 * 下面三个参数只做稳定性防抖：
 * - 弱信号门槛：优先忽略低于该 RSSI 的候选，除非没有更好的候选。
 * - 切换门槛：新候选必须明显强于上次成功连接的 SSID，才允许切走。
 * - 断线延迟：避免 AP 瞬时抖动时马上重扫拿到不完整结果。
 */
#define WIFI_WEAK_RSSI_THRESHOLD_DBM   (-82)
#define WIFI_RSSI_SWITCH_MARGIN_DB     (8)
#define WIFI_FAST_RETRY_DELAY_MS 1000
#define WIFI_BACKGROUND_RETRY_DELAY_MS 10000
#define WIFI_RESCAN_FAILURE_THRESHOLD 2
#define WIFI_STA_FAILURE_LIMIT 5U
#define INTERNET_PROBE_RETRY_INTERVAL_MS 5000
#define INTERNET_PROBE_TIMEOUT_MS 5000
#define INTERNET_PROBE_URL "http://www.qq.com/favicon.ico"

typedef enum {
    WIFI_STA_STATE_IDLE = 0,
    WIFI_STA_STATE_CONNECTING,
    WIFI_STA_STATE_WAIT_IP,
    WIFI_STA_STATE_ONLINE,
    WIFI_STA_STATE_SWITCHING,
} wifi_sta_state_t;

static bool s_inited = false;
static bool s_sta_reconnect_enabled = true;
static bool s_wifi_running = false;
static bool s_sta_connected = false;
static uint32_t s_consecutive_sta_failures;
static wifi_sta_state_t s_sta_state = WIFI_STA_STATE_IDLE;
static portMUX_TYPE s_sta_state_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_config_t s_pending_sta_config;
static bool s_pending_sta_config_valid;
static uint32_t s_connect_generation;
static bool s_ap_started = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static char s_sta_ip_str[16] = "0.0.0.0";
static char s_ap_ip_str[16] = "0.0.0.0";
static char s_sta_ssid[WIFI_SSID_MAX_LEN + 1] = "";
static char s_ap_name[AP_NAME_MAX_LEN + 1] = "ESP32S3-SETUP";
static char s_ap_pass[AP_PASS_MAX_LEN + 1] = "12345678";
static int s_sta_rssi = -127;
static int s_sta_channel = 0;
static int s_ap_channel = 6;
static char s_sta_phy[8] = "N/A";
static wifi_status_cb_t s_status_cb = NULL;
static app_config_t s_saved_cfg;
static wifi_config_t s_active_sta_config;
static bool s_active_sta_config_valid;
static char s_last_connected_ssid[WIFI_SSID_MAX_LEN + 1] = "";
static char s_preferred_fallback_ssid[WIFI_SSID_MAX_LEN + 1] = "";
static bool s_last_connected_seen_in_scan = false;
static wifi_ap_record_t s_last_connected_scan_ap;
static TaskHandle_t s_reconnect_task = NULL;
static TaskHandle_t s_internet_probe_task = NULL;
static internet_health_policy_t s_internet_health;
static portMUX_TYPE s_internet_health_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_network_event_queue;
static TaskHandle_t s_network_coordinator_task;
static bool s_network_coordinator_active;

static void wake_internet_probe_task(void)
{
    if (s_internet_probe_task != NULL) {
        xTaskNotifyGive(s_internet_probe_task);
    }
}

/* Once online, do not create HTTP clients repeatedly. A connection-state
 * event wakes this task immediately while it still feeds its watchdog. */
static void internet_probe_wait_for_connection_change(void)
{
    for (;;) {
        watchdog_service_feed();
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) != 0U) return;
    }
}

static void update_ap_ip_cache(void);
static void schedule_sta_reconnect(void);
static void connect_saved_sta(const app_config_t *cfg, bool force_scan);
static esp_err_t start_sta_with_config(const wifi_config_t *sta_cfg,
                                       const char *context);
static void notify_status_changed(void);

typedef struct {
    size_t body_bytes;
    bool content_type_valid;
    char date_header[64];
} internet_probe_result_t;

static bool has_non_whitespace_text(const char *value)
{
    if (value == NULL) return false;
    while (*value != '\0') {
        if (*value != ' ' && *value != '\t' && *value != '\r' && *value != '\n') {
            return true;
        }
        ++value;
    }
    return false;
}

static esp_err_t internet_probe_http_event(esp_http_client_event_t *event)
{
    watchdog_service_feed();
    if (event == NULL || event->user_data == NULL) return ESP_OK;

    internet_probe_result_t *result = (internet_probe_result_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        result->body_bytes += (size_t)event->data_len;
    } else if (event->event_id == HTTP_EVENT_ON_HEADER &&
               event->header_key != NULL && event->header_value != NULL) {
        if (strcasecmp(event->header_key, "Content-Type") == 0) {
            result->content_type_valid = has_non_whitespace_text(event->header_value);
        } else if (strcasecmp(event->header_key, "Date") == 0) {
            strlcpy(result->date_header, event->header_value, sizeof(result->date_header));
        }
    }
    return ESP_OK;
}

static bool apply_http_date(const char *date_header)
{
    if (date_header == NULL || date_header[0] == '\0') return false;
    char weekday[4] = {0};
    char month_name[4] = {0};
    char zone[4] = {0};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(date_header, "%3[^,], %d %3s %d %d:%d:%d %3s",
               weekday, &day, month_name, &year, &hour, &minute, &second, zone) != 8 ||
        strcmp(zone, "GMT") != 0) return false;
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    int month = -1;
    for (int index = 0; index < 12; ++index) {
        if (strcmp(month_name, months[index]) == 0) {
            month = index + 1;
            break;
        }
    }
    if (month < 1 || day < 1 || day > 31 || year < 1970 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) return false;
    int adjusted_year = year - (month <= 2 ? 1 : 0);
    const int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const unsigned year_of_era = (unsigned)(adjusted_year - era * 400);
    const unsigned day_of_year = (153U * (unsigned)(month + (month > 2 ? -3 : 9)) + 2U) / 5U +
                                 (unsigned)day - 1U;
    const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    const int64_t days_since_epoch = (int64_t)era * 146097LL + (int64_t)day_of_era - 719468LL;
    const time_t utc = (time_t)(days_since_epoch * 86400LL + hour * 3600 + minute * 60 + second);
    if (utc <= 0) return false;
    const struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) return false;
    time_service_apply_http_time(utc);
    APP_LOGI(TAG, "HTTP Date applied: %s", date_header);
    return true;
}

static void reset_internet_health(void)
{
    portENTER_CRITICAL(&s_internet_health_lock);
    internet_health_policy_init(&s_internet_health);
    portEXIT_CRITICAL(&s_internet_health_lock);
}

static internet_health_action_t record_internet_probe(bool probe_ok)
{
    internet_health_action_t action;
    portENTER_CRITICAL(&s_internet_health_lock);
    action = internet_health_policy_record(&s_internet_health, probe_ok);
    portEXIT_CRITICAL(&s_internet_health_lock);
    return action;
}

static bool get_internet_online(void)
{
    bool online;
    portENTER_CRITICAL(&s_internet_health_lock);
    online = s_internet_health.online;
    portEXIT_CRITICAL(&s_internet_health_lock);
    return online;
}

static uint8_t get_internet_probe_failures(void)
{
    uint8_t failures;
    portENTER_CRITICAL(&s_internet_health_lock);
    failures = s_internet_health.consecutive_failures;
    portEXIT_CRITICAL(&s_internet_health_lock);
    return failures;
}

static bool probe_internet_once(void)
{
    internet_probe_result_t response = {0};
    esp_http_client_config_t cfg = {
        .url = INTERNET_PROBE_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = INTERNET_PROBE_TIMEOUT_MS,
        /* Reachability is intentionally checked over HTTP. HTTPS certificate
         * validation cannot be reliable before SNTP establishes system time. */
        .disable_auto_redirect = true,
        .event_handler = internet_probe_http_event,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return false;

    http_memory_log("before", "internet_probe");
    watchdog_service_set_current_task_blocking(true);
    esp_err_t err = esp_http_client_perform(client);
    watchdog_service_set_current_task_blocking(false);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    http_memory_log("after", "internet_probe");
    const bool redirect_received = status >= 300 && status < 400;
    const bool body_received = response.body_bytes > 0 && response.content_type_valid;
    const bool probe_ok = err == ESP_OK && status >= 200 && status < 400 &&
                          (body_received || redirect_received);
    if (!probe_ok) {
        APP_LOGW(TAG, "internet probe failed: err=%s status=%d body=%u content_type=%d",
                 esp_err_to_name(err), status, (unsigned)response.body_bytes,
                 response.content_type_valid);
    }
    if (probe_ok) (void)apply_http_date(response.date_header);
    return probe_ok;
}

static void internet_probe_task(void *arg)
{
    (void)arg;
    (void)watchdog_service_subscribe_current_task();
    for (;;) {
        watchdog_service_feed();
        if (!s_sta_connected) {
            reset_internet_health();
            internet_probe_wait_for_connection_change();
            continue;
        }

        const bool was_online = get_internet_online();
        const bool probe_ok = probe_internet_once();
        const internet_health_action_t action = record_internet_probe(probe_ok);
        if (action == INTERNET_HEALTH_ONLINE && !was_online) {
            APP_LOGI(TAG, "internet probe online: %s", INTERNET_PROBE_URL);
            (void)runtime_event_log_append(RUNTIME_EVENT_INTERNET_PROBE, 1, 0, 0);
            notify_status_changed();
        } else if (action == INTERNET_HEALTH_RECONNECT) {
            APP_LOGW(TAG, "internet probe failed 3 times; reconnecting STA");
            (void)runtime_event_log_append(RUNTIME_EVENT_INTERNET_PROBE, 0, 3, 0);
            notify_status_changed();
            wifi_manager_reconnect_sta();
        }

        watchdog_service_feed();
        /* A successful probe is a startup/reconnect gate, not a five-second
         * background poll. It runs again only after a connection-state event
         * resets the health state, preventing recurring client allocations. */
        if (get_internet_online()) {
            internet_probe_wait_for_connection_change();
            continue;
        }
        internet_probe_delay(INTERNET_PROBE_RETRY_INTERVAL_MS);
    }
}

static const char *wifi_sta_state_to_str(wifi_sta_state_t state)
{
    switch (state) {
    case WIFI_STA_STATE_IDLE:
        return "IDLE";
    case WIFI_STA_STATE_CONNECTING:
        return "CONNECTING";
    case WIFI_STA_STATE_WAIT_IP:
        return "WAIT_IP";
    case WIFI_STA_STATE_ONLINE:
        return "ONLINE";
    case WIFI_STA_STATE_SWITCHING:
        return "SWITCHING";
    default:
        return "UNKNOWN";
    }
}

static wifi_sta_state_t get_sta_state(void)
{
    portENTER_CRITICAL(&s_sta_state_lock);
    wifi_sta_state_t state = s_sta_state;
    portEXIT_CRITICAL(&s_sta_state_lock);
    return state;
}

static uint32_t get_sta_failure_count(void)
{
    portENTER_CRITICAL(&s_sta_state_lock);
    const uint32_t count = s_consecutive_sta_failures;
    portEXIT_CRITICAL(&s_sta_state_lock);
    return count;
}

static bool get_active_sta_config(wifi_config_t *out)
{
    bool valid;
    portENTER_CRITICAL(&s_sta_state_lock);
    valid = s_active_sta_config_valid;
    if (valid && out != NULL) *out = s_active_sta_config;
    portEXIT_CRITICAL(&s_sta_state_lock);
    return valid;
}

static void set_sta_state(wifi_sta_state_t state)
{
    portENTER_CRITICAL(&s_sta_state_lock);
    s_sta_state = state;
    portEXIT_CRITICAL(&s_sta_state_lock);
}

static const char *wifi_disconnect_reason_to_str(int reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "AUTH_EXPIRE";
    case WIFI_REASON_ASSOC_LEAVE:
        return "ASSOC_LEAVE";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        return "DISASSOC_DUE_TO_INACTIVITY";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:
        return "UNKNOWN";
    }
}

static const char *wifi_disconnect_reason_hint(int reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "authentication expired during connection";
    case WIFI_REASON_ASSOC_LEAVE:
        return "station intentionally left the previous AP";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        return "AP dropped link or signal unstable";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "password mismatch, weak signal, or WPA handshake compatibility issue";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "signal too weak or AP not stable";
    case WIFI_REASON_NO_AP_FOUND:
        return "AP not seen in scan; hidden, offline, too weak, or 5GHz-only";
    case WIFI_REASON_AUTH_FAIL:
        return "authentication failed; check password or auth mode";
    case WIFI_REASON_ASSOC_FAIL:
        return "association failed; AP rejected station";
    case WIFI_REASON_CONNECTION_FAIL:
        return "generic connection failure";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "SSID exists but security mode is incompatible";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "SSID exists but below authmode threshold";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "SSID exists but signal below RSSI threshold";
    default:
        return "-";
    }
}
/*
 * 统一状态通知入口。
 * 当前主要用于通知 BLE 模块刷新广播 Manufacturer Data 中的状态位。
 */
static void notify_status_changed(void)
{
    if (s_network_coordinator_active && s_status_cb != NULL) {
        s_status_cb();
        return;
    }
    (void)wifi_manager_post_network_event(&(network_coordinator_event_t){.type = NETWORK_EVENT_STATUS_REFRESH});
}

static void clear_sta_runtime_state(void)
{
    s_sta_connected = false;
    reset_internet_health();
    s_sta_rssi = -127;
    s_sta_channel = 0;
    strlcpy(s_sta_phy, "N/A", sizeof(s_sta_phy));
    s_sta_ssid[0] = '\0';
    strlcpy(s_sta_ip_str, "0.0.0.0", sizeof(s_sta_ip_str));
}

static uint32_t record_sta_connection_failure(const char *context)
{
    portENTER_CRITICAL(&s_sta_state_lock);
    if (s_consecutive_sta_failures < UINT32_MAX) {
        s_consecutive_sta_failures++;
    }
    const uint32_t count = s_consecutive_sta_failures;
    portEXIT_CRITICAL(&s_sta_state_lock);

    APP_LOGW(TAG, "STA consecutive connection failures=%" PRIu32 " context=%s",
             count, context);
    notify_status_changed();
    return count;
}

static void format_mac_string(const uint8_t mac[6], char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    if (mac == NULL) {
        strlcpy(out, "-", out_size);
        return;
    }

    snprintf(out,
             out_size,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void log_ap_network_info(void)
{
    uint8_t ap_mac[6] = {0};
    char ap_mac_str[18] = "-";

    update_ap_ip_cache();
    if (esp_wifi_get_mac(WIFI_IF_AP, ap_mac) == ESP_OK) {
        format_mac_string(ap_mac, ap_mac_str, sizeof(ap_mac_str));
    }

    APP_LOG_STATE(TAG,
                  "AP net: ssid=%s ip=%s channel=%d mac=%s",
                  s_ap_name,
                  s_ap_ip_str,
                  s_ap_channel,
                  ap_mac_str);
}

static void log_sta_network_info(const ip_event_got_ip_t *event)
{
    if (event == NULL) {
        return;
    }

    uint8_t sta_mac[6] = {0};
    uint8_t sta_bssid[6] = {0};
    char sta_mac_str[18] = "-";
    char sta_bssid_str[18] = "-";
    char mask_str[16] = "0.0.0.0";
    char gw_str[16] = "0.0.0.0";
    char dns1_str[16] = "0.0.0.0";
    char dns2_str[16] = "0.0.0.0";

    esp_ip4addr_ntoa(&event->ip_info.netmask, mask_str, sizeof(mask_str));
    esp_ip4addr_ntoa(&event->ip_info.gw, gw_str, sizeof(gw_str));

    if (esp_wifi_get_mac(WIFI_IF_STA, sta_mac) == ESP_OK) {
        format_mac_string(sta_mac, sta_mac_str, sizeof(sta_mac_str));
    }

    if (wifi_manager_get_sta_bssid(sta_bssid)) {
        format_mac_string(sta_bssid, sta_bssid_str, sizeof(sta_bssid_str));
    }

    esp_netif_dns_info_t dns = {0};
    if (s_sta_netif != NULL &&
        esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, dns1_str, sizeof(dns1_str));
    }
    memset(&dns, 0, sizeof(dns));
    if (s_sta_netif != NULL &&
        esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK) {
        esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, dns2_str, sizeof(dns2_str));
    }

    APP_LOG_STATE(TAG,
                  "STA wifi: ssid=%s bssid=%s rssi=%d channel=%d phy=%s sta_mac=%s",
                  s_sta_ssid[0] != '\0' ? s_sta_ssid : "-",
                  sta_bssid_str,
                  s_sta_rssi,
                  s_sta_channel,
                  s_sta_phy,
                  sta_mac_str);
    APP_LOG_STATE(TAG,
                  "STA net: ip=%s mask=%s gw=%s dns1=%s dns2=%s",
                  s_sta_ip_str,
                  mask_str,
                  gw_str,
                  dns1_str,
                  dns2_str);
}

static void sta_reconnect_task(void *arg)
{
    const uint32_t generation = (uint32_t)(uintptr_t)arg;
    const uint32_t failures = get_sta_failure_count();
    const uint32_t delay_ms = failures >= 3U
                                  ? WIFI_BACKGROUND_RETRY_DELAY_MS
                                  : WIFI_FAST_RETRY_DELAY_MS;

    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    const wifi_sta_state_t state = get_sta_state();
    if (generation != s_connect_generation || state != WIFI_STA_STATE_IDLE) {
        APP_LOGI(TAG,
                 "discard stale reconnect: task_gen=%" PRIu32 " current_gen=%" PRIu32 " state=%s",
                 generation,
                 s_connect_generation,
                 wifi_sta_state_to_str(state));
    } else if (s_sta_reconnect_enabled) {
        if (s_saved_cfg.has_wifi && s_saved_cfg.wifi_count > 0) {
            wifi_config_t active_cfg = {0};
            if (failures < WIFI_RESCAN_FAILURE_THRESHOLD &&
                get_active_sta_config(&active_cfg)) {
                APP_LOGI(TAG, "reconnect path: direct retry, failures=%" PRIu32,
                         failures);
                (void)start_sta_with_config(&active_cfg, "direct retry");
            } else {
                APP_LOGI(TAG, "reconnect path: scan saved profiles, failures=%" PRIu32,
                         failures);
                connect_saved_sta(&s_saved_cfg, true);
            }
        } else {
            APP_LOGI(TAG, "reconnect path: delayed esp_wifi_connect only");
            esp_wifi_connect();
        }
    }

    s_reconnect_task = NULL;

    if (s_sta_reconnect_enabled &&
        s_saved_cfg.has_wifi &&
        get_sta_state() == WIFI_STA_STATE_IDLE) {
        schedule_sta_reconnect();
    }
    vTaskDelete(NULL);
}

static void schedule_sta_reconnect(void)
{
    if (s_reconnect_task != NULL) {
        APP_LOGW(TAG, "STA reconnect already scheduled");
        return;
    }

    BaseType_t ok = xTaskCreate(sta_reconnect_task,
                                "sta_reconn",
                                4096,
                                (void *)(uintptr_t)s_connect_generation,
                                5,
                                &s_reconnect_task);
    if (ok != pdPASS) {
        s_reconnect_task = NULL;
        APP_LOGE(TAG, "create STA reconnect task failed");
    }
}
/*
 * 组装 SoftAP 配置。
 * AP 密码长度 >= 8 时使用 WPA2-PSK，否则按开放热点处理。
 */
static void build_ap_config(wifi_config_t *ap_cfg)
{

    memset(ap_cfg, 0, sizeof(*ap_cfg));
    strlcpy((char *)ap_cfg->ap.ssid, s_ap_name, sizeof(ap_cfg->ap.ssid));
    ap_cfg->ap.ssid_len = 0;
    ap_cfg->ap.ssid_hidden = 0;
    ap_cfg->ap.channel = 6;
    ap_cfg->ap.max_connection = 4;
    ap_cfg->ap.beacon_interval = 100;
    strlcpy((char *)ap_cfg->ap.password, s_ap_pass, sizeof(ap_cfg->ap.password));
    ap_cfg->ap.authmode = (strlen(s_ap_pass) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
}

static void update_ap_ip_cache(void)
{
    /* 缓存 AP 接口 IP，方便日志、状态查询和后续 App 查询。 */
    if (s_ap_netif == NULL)
    {
        return;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK)
    {
        esp_ip4addr_ntoa(&ip_info.ip, s_ap_ip_str, sizeof(s_ap_ip_str));
    }
}

static void update_ap_channel_from_config(const wifi_config_t *ap_cfg)
{
    if (ap_cfg == NULL) {
        return;
    }
    s_ap_channel = ap_cfg->ap.channel;
}

static bool refresh_sta_runtime_info(wifi_ap_record_t *ap)
{
    if (!s_sta_connected) {
        return false;
    }

    wifi_ap_record_t local_ap = { 0 };
    if (esp_wifi_sta_get_ap_info(&local_ap) != ESP_OK) {
        return false;
    }

    s_sta_rssi = local_ap.rssi;
    s_sta_channel = local_ap.primary;
    strlcpy(s_sta_ssid, (const char *)local_ap.ssid, sizeof(s_sta_ssid));
    if (local_ap.phy_11n) {
        strlcpy(s_sta_phy, "11n", sizeof(s_sta_phy));
    } else if (local_ap.phy_11g) {
        strlcpy(s_sta_phy, "11g", sizeof(s_sta_phy));
    } else if (local_ap.phy_11b) {
        strlcpy(s_sta_phy, "11b", sizeof(s_sta_phy));
    } else {
        strlcpy(s_sta_phy, "N/A", sizeof(s_sta_phy));
    }

    if (ap != NULL) {
        *ap = local_ap;
    }

    return true;
}

static esp_err_t start_sta_with_config(const wifi_config_t *sta_cfg,
                                       const char *context)
{
    if (sta_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_sta_state_t state = get_sta_state();
    if (state != WIFI_STA_STATE_IDLE) {
        APP_LOGW(TAG, "skip STA config while state=%s, context=%s",
                 wifi_sta_state_to_str(state), context);
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t config = *sta_cfg;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "set STA config failed, context=%s state=%s",
                    context, wifi_sta_state_to_str(get_sta_state()));
        return err;
    }

    portENTER_CRITICAL(&s_sta_state_lock);
    s_active_sta_config = *sta_cfg;
    s_active_sta_config_valid = true;
    s_sta_state = WIFI_STA_STATE_CONNECTING;
    portEXIT_CRITICAL(&s_sta_state_lock);
    strlcpy(s_sta_ssid, (const char *)sta_cfg->sta.ssid, sizeof(s_sta_ssid));
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        set_sta_state(WIFI_STA_STATE_IDLE);
        APP_LOG_ERR(TAG, err, "start STA connect failed, context=%s", context);
        if (s_sta_reconnect_enabled) {
            (void)record_sta_connection_failure("esp_wifi_connect");
        }
        return err;
    }

    APP_LOGI(TAG, "STA connect started, context=%s generation=%" PRIu32,
             context, s_connect_generation);
    return ESP_OK;
}

static bool take_pending_sta_config(wifi_config_t *out)
{
    bool available = false;
    portENTER_CRITICAL(&s_sta_state_lock);
    if (s_pending_sta_config_valid) {
        *out = s_pending_sta_config;
        s_pending_sta_config_valid = false;
        available = true;
    }
    portEXIT_CRITICAL(&s_sta_state_lock);
    return available;
}

static bool apply_pending_sta_config(void)
{
    wifi_config_t sta_cfg = {0};
    if (!take_pending_sta_config(&sta_cfg)) {
        return false;
    }

    APP_LOGI(TAG, "apply pending STA config for SSID=%s",
             (const char *)sta_cfg.sta.ssid);
    esp_err_t err = start_sta_with_config(&sta_cfg, "pending BluFi config");
    if (err != ESP_OK && s_sta_reconnect_enabled) {
        schedule_sta_reconnect();
    }
    return true;
}

static void process_wifi_event(network_coordinator_event_t *network_event)
{
    const network_event_type_t event_id = network_event->type;
    void *event_data = NULL;
    if (event_id == NETWORK_EVENT_STA_DISCONNECTED) event_data = &network_event->data.disconnected;
    if (event_id == NETWORK_EVENT_STA_GOT_IP) event_data = &network_event->data.got_ip;

    if (event_id == NETWORK_EVENT_STA_START)
    {
        /* STA 驱动已启动，不代表已经连接路由器。 */
        set_sta_state(WIFI_STA_STATE_IDLE);
        reset_internet_health();
        APP_LOGI(TAG, "STA start");
    }
    else if (event_id == NETWORK_EVENT_STA_CONNECTED)
    {
        set_sta_state(WIFI_STA_STATE_WAIT_IP);
        APP_LOGI(TAG, "STA link connected, waiting for DHCP");
    }
    else if (event_id == NETWORK_EVENT_STA_DISCONNECTED)
    {
        /*
         * STA 断开：清掉连接状态和 IP 缓存。
         * 如果 s_sta_reconnect_enabled=true，则让 ESP-IDF 继续重连。
         */
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        const wifi_sta_state_t previous_state = get_sta_state();
        const bool count_connection_failure =
            s_sta_reconnect_enabled &&
            previous_state != WIFI_STA_STATE_SWITCHING &&
            (event == NULL || event->reason != WIFI_REASON_ASSOC_LEAVE);
        APP_LOGW(TAG, "STA disconnected, previous_state=%s reason=%d(%s), hint=%s, reconnect=%d, saved=%d",
                 wifi_sta_state_to_str(previous_state),
                 event != NULL ? event->reason : -1,
                 event != NULL ? wifi_disconnect_reason_to_str(event->reason) : "UNKNOWN",
                 event != NULL ? wifi_disconnect_reason_hint(event->reason) : "-",
                 s_sta_reconnect_enabled ? 1 : 0,
                 (s_saved_cfg.has_wifi && s_saved_cfg.wifi_count > 0) ? 1 : 0);
        set_sta_state(WIFI_STA_STATE_IDLE);
        clear_sta_runtime_state();
        wake_internet_probe_task();
        if (count_connection_failure) {
            (void)record_sta_connection_failure("disconnect event");
        } else {
            notify_status_changed();
        }
        if (count_connection_failure) {
            const uint32_t failure_count = get_sta_failure_count();
            /* Persist only the first and third failures; background retries stay in RAM. */
            if (failure_count == 1U || failure_count == 3U) {
                runtime_event_log_append(RUNTIME_EVENT_WIFI_FAILURE,
                                         (int32_t)failure_count,
                                         event != NULL ? (int32_t)event->reason : -1,
                                         0);
            }
        }
        if (apply_pending_sta_config())
        {
            APP_LOGI(TAG, "intentional STA switch continues without delayed reconnect");
        }
        else if (s_sta_reconnect_enabled)
        {
            const uint32_t failures = get_sta_failure_count();
            if (failures >= WIFI_STA_FAILURE_LIMIT) {
                APP_LOGW(TAG, "STA retry limit reached (%u); waiting for BluFi credentials",
                         (unsigned)WIFI_STA_FAILURE_LIMIT);
                wifi_manager_enable_sta_reconnect(false);
            } else {
                const uint32_t delay_ms = failures >= 3U
                                               ? WIFI_BACKGROUND_RETRY_DELAY_MS
                                               : WIFI_FAST_RETRY_DELAY_MS;
                APP_LOGI(TAG, "STA reconnect scheduled in %" PRIu32 " ms", delay_ms);
                schedule_sta_reconnect();
            }
        }
    }
    else if (event_id == NETWORK_EVENT_AP_START)
    {
        /* AP 热点已经启动，手机应能扫描到 s_ap_name。 */
        s_ap_started = true;
        APP_LOGI(TAG, "AP start, ssid=%s", s_ap_name);
        log_ap_network_info();
        notify_status_changed();
    }
    else if (event_id == NETWORK_EVENT_AP_STOP)
    {
        /* AP 热点停止，通常不应该在正常 APSTA 模式下频繁出现。 */
        s_ap_started = false;
        APP_LOGW(TAG, "AP stop");
        notify_status_changed();
    }
    else if (event_id == NETWORK_EVENT_STA_GOT_IP)
    {
        /* STA 已经从上级路由器/手机热点拿到内网 IP，说明 STA 连接成功。 */
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_sta_connected = true;
        reset_internet_health();
        wake_internet_probe_task();
        set_sta_state(WIFI_STA_STATE_ONLINE);
        portENTER_CRITICAL(&s_sta_state_lock);
        s_consecutive_sta_failures = 0;
        portEXIT_CRITICAL(&s_sta_state_lock);
        esp_ip4addr_ntoa(&event->ip_info.ip, s_sta_ip_str, sizeof(s_sta_ip_str));
        APP_LOGI(TAG, "STA got IP: %s", s_sta_ip_str);
        runtime_event_log_append(RUNTIME_EVENT_WIFI_CONNECTED,
                                 s_sta_rssi,
                                 s_sta_channel,
                                 0);
        refresh_sta_runtime_info(NULL);
        strlcpy(s_last_connected_ssid, s_sta_ssid, sizeof(s_last_connected_ssid));
        strlcpy(s_preferred_fallback_ssid, s_sta_ssid, sizeof(s_preferred_fallback_ssid));
        if (!s_saved_cfg.has_last_wifi_ssid ||
            strcmp(s_saved_cfg.last_wifi_ssid, s_sta_ssid) != 0) {
            esp_err_t save_err = app_config_save_last_wifi_ssid(s_sta_ssid);
            if (save_err == ESP_OK) {
                strlcpy(s_saved_cfg.last_wifi_ssid, s_sta_ssid,
                        sizeof(s_saved_cfg.last_wifi_ssid));
                s_saved_cfg.has_last_wifi_ssid = true;
            }
        }
        log_sta_network_info(event);
        notify_status_changed();
    }
    else if (event_id == NETWORK_EVENT_STA_LOST_IP)
    {
        /* DHCP 地址丢失时，STA 链路可能仍保持连接，必须主动清理在线状态。 */
        APP_LOGW(TAG, "STA lost IP; clearing network state and requesting reconnect");
        set_sta_state(WIFI_STA_STATE_WAIT_IP);
        clear_sta_runtime_state();
        notify_status_changed();
        wifi_manager_reconnect_sta();
    }
}

static void network_coordinator_task(void *arg)
{
    (void)arg;
    network_coordinator_event_t event;
    for (;;) {
        if (xQueueReceive(s_network_event_queue, &event, portMAX_DELAY) != pdTRUE) continue;
        APP_LOGI(TAG, "Wi-Fi async dispatch: event=%" PRId32 " stack_watermark=%u",
                 (int32_t)event.type, (unsigned)uxTaskGetStackHighWaterMark(NULL));
        s_network_coordinator_active = true;
        process_wifi_event(&event);
        s_network_coordinator_active = false;
        if (event.type == NETWORK_EVENT_STATUS_REFRESH ||
            event.type == NETWORK_EVENT_SERVICES_READY) {
            if (s_status_cb != NULL) s_status_cb();
        } else {
            ble_provision_handle_network_event(event.type);
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    network_coordinator_event_t event = {0};
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) event.type = NETWORK_EVENT_STA_START;
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) event.type = NETWORK_EVENT_STA_CONNECTED;
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        event.type = NETWORK_EVENT_STA_DISCONNECTED;
        if (event_data != NULL) event.data.disconnected = *(const wifi_event_sta_disconnected_t *)event_data;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) event.type = NETWORK_EVENT_AP_START;
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) event.type = NETWORK_EVENT_AP_STOP;
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) event.type = NETWORK_EVENT_SCAN_DONE;
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        event.type = NETWORK_EVENT_STA_GOT_IP;
        if (event_data != NULL) event.data.got_ip = *(const ip_event_got_ip_t *)event_data;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) event.type = NETWORK_EVENT_STA_LOST_IP;
    else return;
    (void)wifi_manager_post_network_event(&event);
}

bool wifi_manager_post_network_event(const network_coordinator_event_t *event)
{
    if (s_network_event_queue == NULL || event == NULL) return false;
    if (xQueueSend(s_network_event_queue, event, 0) != pdTRUE) {
        APP_LOGW(TAG, "network coordinator queue full: event=%d", event->type);
        return false;
    }
    return true;
}

esp_err_t wifi_manager_init(const app_config_t *cfg)
{
    /*
     * Wi-Fi 初始化只执行一次。
     *
     * 初始化顺序：
     * 1. 从配置恢复 AP 名称/密码。
     * 2. 初始化 esp-netif 和默认事件循环。
     * 3. 创建 STA/AP 两个 netif。
     * 4. 注册 Wi-Fi/IP 事件回调。
     * 5. 设置 APSTA 模式并启动 Wi-Fi。
     */
    if (s_inited)
    {
        return ESP_OK;
    }

    if (cfg != NULL && cfg->has_ap_name && cfg->ap_name[0] != '\0')
    {
        strlcpy(s_ap_name, cfg->ap_name, sizeof(s_ap_name));
    }
    if (cfg != NULL && cfg->has_ap_pass && cfg->ap_pass[0] != '\0')
    {
        strlcpy(s_ap_pass, cfg->ap_pass, sizeof(s_ap_pass));
    }
    if (cfg != NULL) {
        s_saved_cfg = *cfg;
        APP_LOGI(TAG, "wifi manager init: saved_profiles=%d has_wifi=%d ap_name=%s",
                 cfg->wifi_count, cfg->has_wifi ? 1 : 0, s_ap_name);
    } else {
        memset(&s_saved_cfg, 0, sizeof(s_saved_cfg));
        APP_LOGW(TAG, "wifi manager init without config");
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        APP_LOG_ERR(TAG, err, "initialize esp-netif failed");
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        APP_LOG_ERR(TAG, err, "create default event loop failed");
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        APP_LOGE(TAG, "create Wi-Fi netif failed");
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        APP_LOG_ERR(TAG, err, "initialize Wi-Fi driver failed");
        return err;
    }

    s_network_event_queue = xQueueCreate(16U, sizeof(network_coordinator_event_t));
    if (s_network_event_queue == NULL) {
        APP_LOGE(TAG, "create Wi-Fi async dispatch queue failed");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(network_coordinator_task, "network_coordinator", 4096,
                                NULL, 5, &s_network_coordinator_task, 1) != pdPASS) {
        s_network_coordinator_task = NULL;
        vQueueDelete(s_network_event_queue);
        s_network_event_queue = NULL;
        APP_LOGE(TAG, "create Wi-Fi async dispatch task failed");
        return ESP_ERR_NO_MEM;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err != ESP_OK) return err;

    wifi_config_t ap_cfg = {0};
    build_ap_config(&ap_cfg);
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "set APSTA mode failed");
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "set initial AP config failed");
        return err;
    }
    update_ap_channel_from_config(&ap_cfg);
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        APP_LOG_ERR(TAG, err, "start Wi-Fi failed");
        return err;
    }

    s_wifi_running = true;
    update_ap_ip_cache();
    s_inited = true;
    internet_health_policy_init(&s_internet_health);
    if (xTaskCreate(internet_probe_task, "internet_probe", 4096, NULL, 3,
                    &s_internet_probe_task) != pdPASS) {
        s_internet_probe_task = NULL;
        APP_LOGE(TAG, "create internet probe task failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
/*
 * 动态修改 AP 名称。
 * 注意：如果手机已经连着旧 AP，改名后可能需要手机重新扫描/重连。
 */
void wifi_manager_set_ap_name(const char *ap_name)
{

    if (ap_name == NULL || ap_name[0] == '\0')
    {
        return;
    }

    if (strncmp(s_ap_name, ap_name, sizeof(s_ap_name)) == 0)
    {
        return;
    }

    char previous_name[sizeof(s_ap_name)];
    strlcpy(previous_name, s_ap_name, sizeof(previous_name));
    strlcpy(s_ap_name, ap_name, sizeof(s_ap_name));
    wifi_config_t ap_cfg = {0};
    build_ap_config(&ap_cfg);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        strlcpy(s_ap_name, previous_name, sizeof(s_ap_name));
        APP_LOG_ERR(TAG, err, "set AP name failed");
        return;
    }
    update_ap_channel_from_config(&ap_cfg);

    if (!s_wifi_running)
    {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            APP_LOG_ERR(TAG, err, "set APSTA mode while applying AP name failed");
            return;
        }
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
            APP_LOG_ERR(TAG, err, "start Wi-Fi while applying AP name failed");
            return;
        }
        s_wifi_running = true;
    }

    update_ap_ip_cache();
}

void wifi_manager_set_ap_password(const char *ap_pass)
{
    /*
     * 动态修改 AP 密码。
     * WPA/WPA2 密码要求至少 8 位，因此短密码直接忽略。
     */
    if (ap_pass == NULL || strlen(ap_pass) < 8)
    {
        return;
    }

    if (strncmp(s_ap_pass, ap_pass, sizeof(s_ap_pass)) == 0)
    {
        return;
    }

    char previous_password[sizeof(s_ap_pass)];
    strlcpy(previous_password, s_ap_pass, sizeof(previous_password));
    strlcpy(s_ap_pass, ap_pass, sizeof(s_ap_pass));
    wifi_config_t ap_cfg = {0};
    build_ap_config(&ap_cfg);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        strlcpy(s_ap_pass, previous_password, sizeof(s_ap_pass));
        APP_LOG_ERR(TAG, err, "set AP password failed");
    }
}

static int score_scan_result(const wifi_ap_record_t *ap)
{
    /* 当前评分只看 RSSI。后续可以叠加认证方式、黑名单、历史成功率等因素。 */
    if (ap == NULL)
    {
        return INT_MIN;
    }

    return ap->rssi;
}

static bool find_best_saved_profile(const app_config_t *cfg, wifi_ap_record_t *best_ap, wifi_profile_t *best_profile)
{
    /*
     * 选择 STA 要连接的 Wi-Fi：
     * 1. 主动扫描当前环境里的全部 AP。
     * 2. 将扫描结果和 NVS 中保存的 SSID 列表做匹配。
     * 3. 在匹配项里选择 RSSI 最大的一项。
     */
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    const TickType_t scan_started = xTaskGetTickCount();
    APP_LOGI(TAG, "saved-profile scan started");
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK)
    {
        return false;
    }
    APP_LOGI(TAG, "saved-profile scan completed in %" PRIu32 " ms",
             (uint32_t)((xTaskGetTickCount() - scan_started) * portTICK_PERIOD_MS));

    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0)
    {
        return false;
    }

    wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (records == NULL)
    {
        return false;
    }

    if (esp_wifi_scan_get_ap_records(&ap_count, records) != ESP_OK)
    {
        free(records);
        return false;
    }

    int best_score = INT_MIN;
    int best_usable_score = INT_MIN;
    bool found = false;
    bool found_usable = false;
    bool preferred_found = false;
    wifi_ap_record_t fallback_ap = {0};
    wifi_profile_t fallback_profile = {0};
    wifi_ap_record_t preferred_ap = {0};
    wifi_profile_t preferred_profile = {0};
    s_last_connected_seen_in_scan = false;
    memset(&s_last_connected_scan_ap, 0, sizeof(s_last_connected_scan_ap));

    for (int i = 0; i < cfg->wifi_count; ++i)
    {
        if (!cfg->wifi_profiles[i].valid || cfg->wifi_profiles[i].wifi_ssid[0] == '\0')
        {
            continue;
        }

        for (int j = 0; j < ap_count; ++j)
        {
            if (strcmp((char *)records[j].ssid, cfg->wifi_profiles[i].wifi_ssid) != 0)
            {
                continue;
            }

            if (s_last_connected_ssid[0] != '\0' &&
                strcmp((char *)records[j].ssid, s_last_connected_ssid) == 0)
            {
                if (!s_last_connected_seen_in_scan ||
                    records[j].rssi > s_last_connected_scan_ap.rssi) {
                    s_last_connected_scan_ap = records[j];
                }
                s_last_connected_seen_in_scan = true;
            }

            int score = score_scan_result(&records[j]);
            if (s_preferred_fallback_ssid[0] != '\0' &&
                strcmp((char *)records[j].ssid, s_preferred_fallback_ssid) == 0) {
                if (!preferred_found || score > score_scan_result(&preferred_ap)) {
                    preferred_ap = records[j];
                    preferred_profile = cfg->wifi_profiles[i];
                    preferred_found = true;
                }
            }

            if (!found || score > best_score)
            {
                best_score = score;
                fallback_ap = records[j];
                fallback_profile = cfg->wifi_profiles[i];
                found = true;
            }

            if (records[j].rssi >= WIFI_WEAK_RSSI_THRESHOLD_DBM &&
                (!found_usable || score > best_usable_score))
            {
                best_usable_score = score;
                *best_ap = records[j];
                *best_profile = cfg->wifi_profiles[i];
                found_usable = true;
            }
        }
    }

    if (preferred_found) {
        APP_LOGI(TAG,
                 "prefer current target SSID=%s over strongest saved candidate",
                 preferred_profile.wifi_ssid);
        *best_ap = preferred_ap;
        *best_profile = preferred_profile;
        free(records);
        return true;
    }

    if (!found_usable && found) {
        APP_LOGW(TAG, "all matched Wi-Fi signals are weak, fallback best SSID=%s RSSI=%d threshold=%d",
                 fallback_profile.wifi_ssid,
                 fallback_ap.rssi,
                 WIFI_WEAK_RSSI_THRESHOLD_DBM);
        *best_ap = fallback_ap;
        *best_profile = fallback_profile;
    }

    free(records);
    return found;
}

void wifi_manager_connect_specific_sta(const app_config_t *cfg,
                                       const char *ssid,
                                       const char *pass,
                                       const uint8_t *bssid,
                                       bool bssid_set)
{
    if (cfg != NULL) {
        s_saved_cfg = *cfg;
    }
    if (ssid != NULL && ssid[0] != '\0') {
        strlcpy(s_preferred_fallback_ssid, ssid, sizeof(s_preferred_fallback_ssid));
    }

    if (ssid == NULL || pass == NULL || ssid[0] == '\0') {
        APP_LOGW(TAG, "skip specific STA connect, invalid credentials");
        return;
    }

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    if (bssid_set && bssid != NULL) {
        memcpy(sta_cfg.sta.bssid, bssid, sizeof(sta_cfg.sta.bssid));
        sta_cfg.sta.bssid_set = 1;
    }

    APP_LOGI(TAG,
             "queue direct STA switch to SSID=%s bssid_set=%d",
             ssid,
             bssid_set ? 1 : 0);
    portENTER_CRITICAL(&s_sta_state_lock);
    s_consecutive_sta_failures = 0;
    portEXIT_CRITICAL(&s_sta_state_lock);
    s_sta_reconnect_enabled = true;
    strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
    s_sta_rssi = -127;
    s_sta_channel = 0;
    strlcpy(s_sta_phy, "N/A", sizeof(s_sta_phy));

    portENTER_CRITICAL(&s_sta_state_lock);
    s_pending_sta_config = sta_cfg;
    s_pending_sta_config_valid = true;
    s_connect_generation++;
    wifi_sta_state_t previous_state = s_sta_state;
    if (s_sta_state != WIFI_STA_STATE_IDLE) {
        s_sta_state = WIFI_STA_STATE_SWITCHING;
    }
    portEXIT_CRITICAL(&s_sta_state_lock);

    if (previous_state == WIFI_STA_STATE_IDLE) {
        apply_pending_sta_config();
        return;
    }

    if (previous_state == WIFI_STA_STATE_SWITCHING) {
        APP_LOGI(TAG, "replace pending STA config while switch is already in progress");
        return;
    }

    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_OK) {
        APP_LOGI(TAG, "waiting for STA disconnect before applying new config");
        return;
    }

    APP_LOG_ERR(TAG, err, "STA disconnect for config switch did not start; apply immediately");
    set_sta_state(WIFI_STA_STATE_IDLE);
    apply_pending_sta_config();
}

static void connect_saved_sta(const app_config_t *cfg, bool force_scan)
{
    /*
     * 发起 STA 连接。
     * cfg 可以保存多组 Wi-Fi；这里不会按保存顺序盲连，而是先扫描，再选择信号最强的已保存热点。
     */
    if (cfg == NULL || !cfg->has_wifi || cfg->wifi_count <= 0)
    {
        APP_LOGW(TAG, "skip STA connect, no wifi profile");
        return;
    }

    wifi_sta_state_t state = get_sta_state();
    if (state != WIFI_STA_STATE_IDLE) {
        APP_LOGI(TAG, "skip saved-profile connect while state=%s",
                 wifi_sta_state_to_str(state));
        return;
    }

    if (cfg != &s_saved_cfg) {
        s_saved_cfg = *cfg;
    }
    cfg = &s_saved_cfg;

    portENTER_CRITICAL(&s_sta_state_lock);
    const uint32_t request_generation = ++s_connect_generation;
    portEXIT_CRITICAL(&s_sta_state_lock);

    wifi_ap_record_t best_ap = {0};
    wifi_profile_t best_profile = {0};
    if (!force_scan) {
        const char *preferred_ssid = cfg->has_last_wifi_ssid
                                         ? cfg->last_wifi_ssid
                                         : NULL;
        for (int i = 0; i < cfg->wifi_count; ++i) {
            if (!cfg->wifi_profiles[i].valid) continue;
            if ((preferred_ssid != NULL &&
                 strcmp(cfg->wifi_profiles[i].wifi_ssid, preferred_ssid) == 0) ||
                (preferred_ssid == NULL && cfg->wifi_count == 1)) {
                best_profile = cfg->wifi_profiles[i];
                APP_LOGI(TAG, "fast connect saved SSID=%s without pre-scan",
                         best_profile.wifi_ssid);
                goto connect_sta;
            }
        }
    }

    if (!find_best_saved_profile(cfg, &best_ap, &best_profile))
    {
        /*
         * 启动期如果扫描暂时没命中，不要直接放弃。
         * 先回退到第一条有效保存配置做一次直连，让驱动侧继续完成后续扫描和重连。
         */
        for (int i = 0; i < cfg->wifi_count; ++i) {
            if (s_preferred_fallback_ssid[0] != '\0' &&
                cfg->wifi_profiles[i].valid &&
                strcmp(cfg->wifi_profiles[i].wifi_ssid, s_preferred_fallback_ssid) == 0) {
                best_profile = cfg->wifi_profiles[i];
                APP_LOGW(TAG,
                         "scan miss, fallback prefer SSID=%s; AP may be hidden/offline/weak or 5GHz-only",
                         best_profile.wifi_ssid);
                goto connect_sta;
            }
        }

        for (int i = 0; i < cfg->wifi_count; ++i) {
            if (cfg->wifi_profiles[i].valid && cfg->wifi_profiles[i].wifi_ssid[0] != '\0') {
                best_profile = cfg->wifi_profiles[i];
                APP_LOGW(TAG,
                         "scan miss, fallback connect SSID=%s; AP may be hidden/offline/weak or 5GHz-only",
                         best_profile.wifi_ssid);
                goto connect_sta;
            }
        }

        APP_LOGW(TAG, "no valid saved wifi profile available");
        return;
    }

    if (s_last_connected_ssid[0] != '\0' &&
        s_last_connected_seen_in_scan &&
        strcmp(best_profile.wifi_ssid, s_last_connected_ssid) != 0 &&
        best_ap.rssi < s_last_connected_scan_ap.rssi + WIFI_RSSI_SWITCH_MARGIN_DB)
    {
        for (int i = 0; i < cfg->wifi_count; ++i) {
            if (cfg->wifi_profiles[i].valid &&
                strcmp(cfg->wifi_profiles[i].wifi_ssid, s_last_connected_ssid) == 0) {
                APP_LOGI(TAG,
                         "keep previous SSID=%s, candidate=%s rssi=%d previous_rssi=%d margin=%d",
                         s_last_connected_ssid,
                         best_profile.wifi_ssid,
                         best_ap.rssi,
                         s_last_connected_scan_ap.rssi,
                         WIFI_RSSI_SWITCH_MARGIN_DB);
                best_profile = cfg->wifi_profiles[i];
                best_ap = s_last_connected_scan_ap;
                break;
            }
        }
    }

connect_sta:
    if (request_generation != s_connect_generation ||
        get_sta_state() != WIFI_STA_STATE_IDLE) {
        APP_LOGI(TAG,
                 "discard saved-profile result: request_gen=%" PRIu32 " current_gen=%" PRIu32 " state=%s",
                 request_generation,
                 s_connect_generation,
                 wifi_sta_state_to_str(get_sta_state()));
        return;
    }

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, best_profile.wifi_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, best_profile.wifi_pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    APP_LOGI(TAG, "connecting STA to SSID=%s RSSI=%d channel=%d",
             best_profile.wifi_ssid,
             best_ap.rssi != 0 ? best_ap.rssi : -127,
             best_ap.primary);
    s_sta_rssi = (best_ap.rssi != 0) ? best_ap.rssi : -127;
    s_sta_channel = best_ap.primary;
    if (best_ap.phy_11n) {
        strlcpy(s_sta_phy, "11n", sizeof(s_sta_phy));
    } else if (best_ap.phy_11g) {
        strlcpy(s_sta_phy, "11g", sizeof(s_sta_phy));
    } else if (best_ap.phy_11b) {
        strlcpy(s_sta_phy, "11b", sizeof(s_sta_phy));
    } else {
        strlcpy(s_sta_phy, "N/A", sizeof(s_sta_phy));
    }
    strlcpy(s_sta_ssid, best_profile.wifi_ssid, sizeof(s_sta_ssid));
    s_sta_reconnect_enabled = true;
    esp_err_t err = start_sta_with_config(&sta_cfg, "saved profile");
    if (err != ESP_OK &&
        request_generation == s_connect_generation &&
        s_sta_reconnect_enabled) {
        schedule_sta_reconnect();
    }
}

void wifi_manager_connect_sta(const app_config_t *cfg)
{
    connect_saved_sta(cfg, false);
}

void wifi_manager_enable_sta_reconnect(bool enable)
{
    s_sta_reconnect_enabled = enable;
    if (!enable) {
        portENTER_CRITICAL(&s_sta_state_lock);
        s_connect_generation++;
        portEXIT_CRITICAL(&s_sta_state_lock);
    }
}

void wifi_manager_set_status_cb(wifi_status_cb_t cb)
{
    s_status_cb = cb;
}

void wifi_manager_disconnect_sta(void)
{
    /*
     * 断开 STA 并清空驱动里的 STA 配置。
     * 这样 CLEAR_WIFI 后不会因为 ESP-IDF 内部仍保留旧配置而自动连回旧热点。
     */
    s_sta_reconnect_enabled = false;
    s_sta_connected = false;
    portENTER_CRITICAL(&s_sta_state_lock);
    s_consecutive_sta_failures = 0;
    s_pending_sta_config_valid = false;
    s_active_sta_config_valid = false;
    s_connect_generation++;
    wifi_sta_state_t previous_state = s_sta_state;
    if (s_sta_state != WIFI_STA_STATE_IDLE) {
        s_sta_state = WIFI_STA_STATE_SWITCHING;
    }
    portEXIT_CRITICAL(&s_sta_state_lock);
    s_sta_rssi = -127;
    s_sta_channel = 0;
    strlcpy(s_sta_phy, "N/A", sizeof(s_sta_phy));
    s_sta_ssid[0] = '\0';
    s_last_connected_ssid[0] = '\0';
    s_preferred_fallback_ssid[0] = '\0';
    strlcpy(s_sta_ip_str, "0.0.0.0", sizeof(s_sta_ip_str));
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        set_sta_state(WIFI_STA_STATE_IDLE);
        APP_LOG_ERR(TAG, err, "disconnect STA while clearing configuration failed");
    } else if (previous_state == WIFI_STA_STATE_IDLE) {
        set_sta_state(WIFI_STA_STATE_IDLE);
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &(wifi_config_t){0});
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "clear runtime STA configuration failed");
    }
}

void wifi_manager_reconnect_sta(void)
{
    /*
     * 主动触发一次 STA 重连。
     * 适用于：STA 还在，但公网访问连续失败，希望重新挂一次 Wi-Fi 链路。
     */
    if (!s_inited || !s_wifi_running) {
        APP_LOGW(TAG, "skip force reconnect, wifi not ready");
        return;
    }

    if (!s_saved_cfg.has_wifi || s_saved_cfg.wifi_count <= 0) {
        APP_LOGW(TAG, "skip force reconnect, no saved wifi profile");
        return;
    }

    APP_LOGW(TAG, "force STA reconnect requested");
    s_sta_reconnect_enabled = true;

    wifi_sta_state_t state = get_sta_state();
    if (state == WIFI_STA_STATE_IDLE) {
        wifi_manager_connect_sta(&s_saved_cfg);
    } else if (state == WIFI_STA_STATE_ONLINE || state == WIFI_STA_STATE_WAIT_IP) {
        portENTER_CRITICAL(&s_sta_state_lock);
        s_connect_generation++;
        s_sta_state = WIFI_STA_STATE_SWITCHING;
        portEXIT_CRITICAL(&s_sta_state_lock);
        esp_err_t err = esp_wifi_disconnect();
        if (err != ESP_OK) {
            set_sta_state(WIFI_STA_STATE_IDLE);
            APP_LOG_ERR(TAG, err, "force STA disconnect failed");
            schedule_sta_reconnect();
        }
    } else {
        APP_LOGI(TAG, "skip force reconnect while state=%s",
                 wifi_sta_state_to_str(state));
    }
}

bool wifi_manager_is_sta_connected(void)
{
    return s_sta_connected;
}

bool wifi_manager_is_sta_reconnect_enabled(void)
{
    return s_sta_reconnect_enabled;
}

bool wifi_manager_is_internet_online(void)
{
    return get_internet_online();
}

uint8_t wifi_manager_get_internet_probe_failures(void)
{
    return get_internet_probe_failures();
}

bool wifi_manager_is_ap_started(void)
{
    return s_ap_started;
}

uint32_t wifi_manager_get_sta_failure_count(void)
{
    return get_sta_failure_count();
}

int wifi_manager_get_sta_rssi(void)
{
    refresh_sta_runtime_info(NULL);
    return s_sta_connected ? s_sta_rssi : -127;
}

int wifi_manager_get_sta_channel(void)
{
    refresh_sta_runtime_info(NULL);
    return s_sta_connected ? s_sta_channel : 0;
}

const char *wifi_manager_get_sta_phy_string(void)
{
    refresh_sta_runtime_info(NULL);
    return s_sta_phy;
}

int wifi_manager_get_ap_channel(void)
{
    return s_ap_started ? s_ap_channel : 0;
}

bool wifi_manager_get_sta_ip(esp_ip4_addr_t *ip)
{
    if (ip == NULL || s_sta_netif == NULL)
    {
        return false;
    }

    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK)
    {
        return false;
    }

    *ip = info.ip;
    return true;
}

bool wifi_manager_get_ap_ip(esp_ip4_addr_t *ip)
{
    if (ip == NULL || s_ap_netif == NULL)
    {
        return false;
    }

    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &info) != ESP_OK)
    {
        return false;
    }

    *ip = info.ip;
    return true;
}

bool wifi_manager_get_sta_mac(uint8_t mac[6])
{
    return mac != NULL && esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK;
}

bool wifi_manager_get_ap_mac(uint8_t mac[6])
{
    return mac != NULL && esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK;
}

bool wifi_manager_get_sta_bssid(uint8_t mac[6])
{
    if (mac == NULL || !s_sta_connected)
    {
        return false;
    }

    wifi_ap_record_t ap = { 0 };
    if (!refresh_sta_runtime_info(&ap))
    {
        return false;
    }

    memcpy(mac, ap.bssid, sizeof(ap.bssid));
    return true;
}

const char *wifi_manager_get_sta_ssid(void)
{
    refresh_sta_runtime_info(NULL);
    return s_sta_ssid;
}

const char *wifi_manager_get_ap_name(void)
{
    return s_ap_name;
}

const char *wifi_manager_get_sta_ip_str(void)
{
    return s_sta_ip_str;
}

const char *wifi_manager_get_ap_ip_str(void)
{
    return s_ap_ip_str;
}
