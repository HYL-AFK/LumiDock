#include "app_time.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "app_weather.h"
#include "app_log.h"
#include "http_memory_log.h"
#include "location_snapshot.h"
#include "time_snapshot.h"
#include "runtime_event_log.h"
#include "wifi_manager.h"
#include "watchdog_service.h"

#define TIME_NET_EVENT BIT0
#define LOCATION_BODY_MAX 2048
#define LOCATION_CHANGE_THRESHOLD_DEGREES 0.05
#define LOCATION_REFRESH_INTERVAL_MS (12 * 60 * 60 * 1000)
#define SNTP_ATTEMPT_TIMEOUT_MS 5000
#define SNTP_RETRY_DELAY_MS 1000
#define SNTP_MAX_ATTEMPTS 3U

static EventGroupHandle_t s_sntp_events;
static EventGroupHandle_t s_location_events;
static char s_location_body[LOCATION_BODY_MAX];
static size_t s_location_len;
static bool s_location_body_overflow;
static bool s_sntp_initialized;
static bool s_sntp_sync_applied;
static bool s_timezone_prepared;
static int64_t s_sntp_started_us;
static uint8_t s_sntp_attempt_count;
static int64_t s_sntp_retry_at_us;
static bool s_sntp_retry_exhausted_logged;
static volatile bool s_initial_location_settled;
static TaskHandle_t s_sntp_task_handle;
static TaskHandle_t s_location_task_handle;
static time_t s_pending_http_time;
static const char *TAG = "app_time";

esp_err_t app_time_prepare_timezone(void)
{
    if (s_timezone_prepared) return ESP_OK;
    if (setenv("TZ", "CST-8", 1) != 0) {
        APP_LOGE(TAG, "set default timezone failed");
        return ESP_FAIL;
    }
    tzset();
    s_timezone_prepared = true;
    return ESP_OK;
}

static esp_err_t app_time_apply_timezone_offset(int32_t offset_seconds)
{
    const int32_t absolute_seconds = offset_seconds >= 0
                                         ? offset_seconds
                                         : -offset_seconds;
    const int hours = absolute_seconds / 3600;
    const int minutes = (absolute_seconds % 3600) / 60;
    char tz[24];
    (void)snprintf(tz, sizeof(tz), "UTC%c%d:%02d",
                   offset_seconds >= 0 ? '-' : '+', hours, minutes);
    if (setenv("TZ", tz, 1) != 0) {
        APP_LOGE(TAG, "set runtime timezone failed, offset=%ld", (long)offset_seconds);
        return ESP_FAIL;
    }
    tzset();
    s_timezone_prepared = true;
    APP_LOGI(TAG, "runtime timezone applied: offset=%ld TZ=%s",
             (long)offset_seconds, tz);
    return ESP_OK;
}

static esp_err_t location_http_event(esp_http_client_event_t *event)
{
    watchdog_service_feed();
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL ||
        event->data_len == 0) {
        return ESP_OK;
    }

    if (s_location_len + event->data_len >= sizeof(s_location_body)) {
        s_location_body_overflow = true;
        return ESP_OK;
    }

    memcpy(s_location_body + s_location_len, event->data, event->data_len);
    s_location_len += event->data_len;
    s_location_body[s_location_len] = '\0';
    return ESP_OK;
}

static bool get_location_value(cJSON *root, const char *name, double *out)
{
    cJSON *value = cJSON_GetObjectItem(root, name);
    if (value == NULL || !cJSON_IsNumber(value) || !isfinite(value->valuedouble)) {
        return false;
    }
    *out = value->valuedouble;
    return true;
}

static const char *get_location_timezone(cJSON *root)
{
    cJSON *timezone = cJSON_GetObjectItem(root, "timezone");
    if (timezone != NULL && cJSON_IsString(timezone)) {
        return timezone->valuestring;
    }
    if (timezone != NULL && cJSON_IsObject(timezone)) {
        cJSON *id = cJSON_GetObjectItem(timezone, "id");
        if (id != NULL && cJSON_IsString(id)) return id->valuestring;
    }
    return NULL;
}

static bool parse_utc_offset_text(const char *text, int32_t *out_seconds)
{
    if (text == NULL || out_seconds == NULL) return false;

    const char *sign_text = text;
    while (*sign_text != '\0' && *sign_text != '+' && *sign_text != '-') ++sign_text;
    if (*sign_text == '\0') return false;

    const int sign = *sign_text == '-' ? -1 : 1;
    int hours = 0;
    int minutes = 0;
    if (sscanf(sign_text + 1, "%2d:%2d", &hours, &minutes) == 2) {
        *out_seconds = sign * (hours * 3600 + minutes * 60);
        return true;
    }

    char digits[5] = {0};
    for (int i = 0; i < 4; ++i) {
        if (sign_text[1 + i] < '0' || sign_text[1 + i] > '9') return false;
        digits[i] = sign_text[1 + i];
    }
    hours = (digits[0] - '0') * 10 + digits[1] - '0';
    minutes = (digits[2] - '0') * 10 + digits[3] - '0';
    *out_seconds = sign * (hours * 3600 + minutes * 60);
    return true;
}

static bool get_location_offset_seconds(cJSON *root, int32_t *out_seconds)
{
    if (root == NULL || out_seconds == NULL) return false;

    cJSON *timezone = cJSON_GetObjectItem(root, "timezone");
    if (timezone != NULL && cJSON_IsObject(timezone)) {
        cJSON *offset = cJSON_GetObjectItem(timezone, "offset");
        if (offset != NULL && cJSON_IsNumber(offset) && isfinite(offset->valuedouble)) {
            *out_seconds = (int32_t)offset->valuedouble;
            return true;
        }
        cJSON *utc = cJSON_GetObjectItem(timezone, "utc");
        if (utc != NULL && cJSON_IsString(utc) &&
            parse_utc_offset_text(utc->valuestring, out_seconds)) {
            return true;
        }
    }

    cJSON *utc_offset = cJSON_GetObjectItem(root, "utc_offset");
    return utc_offset != NULL && cJSON_IsString(utc_offset) &&
           parse_utc_offset_text(utc_offset->valuestring, out_seconds);
}

static bool parse_location_response(bool *changed)
{
    cJSON *root = cJSON_Parse(s_location_body);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        APP_LOGW(TAG, "location JSON parse failed");
        return false;
    }

    cJSON *success = cJSON_GetObjectItem(root, "success");
    if (success != NULL && cJSON_IsFalse(success)) {
        cJSON_Delete(root);
        APP_LOGW(TAG, "location provider reported failure");
        return false;
    }

    double latitude = 0.0;
    double longitude = 0.0;
    const char *timezone = get_location_timezone(root);
    int32_t offset_seconds = 0;
    const bool offset_valid = get_location_offset_seconds(root, &offset_seconds);
    bool valid = get_location_value(root, "latitude", &latitude) &&
                 get_location_value(root, "longitude", &longitude) &&
                 timezone != NULL && timezone[0] != '\0' &&
                 latitude >= -90.0 && latitude <= 90.0 &&
                 longitude >= -180.0 && longitude <= 180.0;
    if (!valid) {
        if (changed != NULL) *changed = false;
        APP_LOGW(TAG, "location JSON missing valid coordinates or timezone");
        cJSON_Delete(root);
        return false;
    }

    location_snapshot_t previous = {0};
    const bool had_previous = location_snapshot_get(&previous);
    const bool location_changed =
        !had_previous ||
        fabs(latitude - previous.latitude) >= LOCATION_CHANGE_THRESHOLD_DEGREES ||
        fabs(longitude - previous.longitude) >= LOCATION_CHANGE_THRESHOLD_DEGREES ||
        strcmp(previous.timezone, timezone) != 0;

    bool ready = location_snapshot_set(latitude, longitude, timezone) == ESP_OK;
    if (ready) {
        APP_LOGI(TAG, "location updated: latitude=%.6f longitude=%.6f timezone=%s",
                 latitude, longitude, timezone);
        if (!offset_valid || app_time_apply_timezone_offset(offset_seconds) != ESP_OK) {
            APP_LOGW(TAG, "location provider did not return a usable UTC offset; keep current TZ");
        }
    } else {
        APP_LOGW(TAG, "location JSON missing valid coordinates or timezone");
    }

    if (changed != NULL) *changed = ready && location_changed;
    cJSON_Delete(root);
    return ready;
}

static bool request_location(const char *url, bool *changed)
{
    s_location_len = 0;
    s_location_body[0] = '\0';
    s_location_body_overflow = false;

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = location_http_event,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        APP_LOGE(TAG, "create location HTTPS client failed, url=%s", url);
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", "ESPARK/1.0");
    http_memory_log("before", "location");
    watchdog_service_set_current_task_blocking(true);
    esp_err_t err = esp_http_client_perform(client);
    watchdog_service_set_current_task_blocking(false);
    int status = esp_http_client_get_status_code(client);
    bool ready = false;
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "location request failed, status=%d url=%s", status, url);
    } else if (status != 200) {
        APP_LOGW(TAG, "location request returned HTTP %d url=%s", status, url);
    } else if (s_location_body_overflow) {
        APP_LOGE(TAG, "location response overflow, status=%d url=%s", status, url);
    } else {
        ready = parse_location_response(changed);
    }

    esp_http_client_cleanup(client);
    http_memory_log("after", "location");
    return ready;
}

/* 主接口拒绝访问时使用备用接口，避免天气功能依赖单一定位服务。 */
static bool refresh_location(bool *changed)
{
    /* 优先使用主定位服务，失败后切换备用服务；已有位置缓存继续保留。 */
    if (changed != NULL) *changed = false;
    if (request_location("https://ipwho.is/", changed) ||
        request_location("https://ipapi.co/json/", changed)) {
        return true;
    }

    location_snapshot_t old = {0};
    return location_snapshot_get(&old);
}

static void apply_sntp_time(void)
{
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    time_snapshot_t snap = {
        .local_time = local,
        .unix_time = now,
        .from_rtc = false,
        .from_sntp = true,
    };
    time_snapshot_set(&snap);
    s_sntp_sync_applied = true;
    /* TLS requests require a valid clock. Location starts only after this point. */
    if (s_location_events != NULL) xEventGroupSetBits(s_location_events, TIME_NET_EVENT);
    const int64_t elapsed_ms = s_sntp_started_us > 0
                                   ? (esp_timer_get_time() - s_sntp_started_us) / 1000
                                   : 0;
    APP_LOGI(TAG, "SNTP sync applied after %lld ms: %04d-%02d-%02d %02d:%02d:%02d",
             (long long)elapsed_ms,
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
    runtime_event_log_append(RUNTIME_EVENT_SNTP_SYNC,
                             (int32_t)elapsed_ms,
                             (int32_t)now,
                             0);
}

void app_time_apply_http_time(time_t utc_time)
{
    if (utc_time <= 0) return;
    s_pending_http_time = utc_time;
    if (s_sntp_events == NULL) return;
    struct tm local;
    localtime_r(&utc_time, &local);
    const time_snapshot_t snap = {
        .local_time = local,
        .unix_time = utc_time,
        .from_rtc = false,
        .from_sntp = false,
    };
    time_snapshot_set(&snap);
    APP_LOGI(TAG, "HTTP Date time applied: %04d-%02d-%02d %02d:%02d:%02d",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
}

void app_time_notify_internet_online(void)
{
    if (s_location_events != NULL) xEventGroupSetBits(s_location_events, TIME_NET_EVENT);
}

static void schedule_sntp_retry(const char *reason)
{
    if (s_sntp_attempt_count < SNTP_MAX_ATTEMPTS) {
        s_sntp_retry_at_us = esp_timer_get_time() +
                             (int64_t)SNTP_RETRY_DELAY_MS * 1000;
        APP_LOGW(TAG,
                 "SNTP attempt failed (%s); retry %u/%u in %d ms",
                 reason,
                 (unsigned)s_sntp_attempt_count,
                 (unsigned)(SNTP_MAX_ATTEMPTS - 1U),
                 SNTP_RETRY_DELAY_MS);
    } else if (!s_sntp_retry_exhausted_logged) {
        s_sntp_retry_exhausted_logged = true;
        APP_LOGW(TAG,
                 "SNTP sync unavailable after %u attempts; keep current system time",
                 (unsigned)SNTP_MAX_ATTEMPTS);
    }
}

static esp_err_t start_sntp_attempt(void)
{
    if (s_sntp_attempt_count >= SNTP_MAX_ATTEMPTS) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *primary_server = "ntp.aliyun.com";
    const char *fallback_server = "ntp.tencent.com";
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST(primary_server, fallback_server));
    s_sntp_attempt_count++;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        s_sntp_initialized = false;
        APP_LOG_ERR(TAG, err, "SNTP init failed, attempt=%u/%u",
                    (unsigned)s_sntp_attempt_count,
                    (unsigned)SNTP_MAX_ATTEMPTS);
        return err;
    }

    s_sntp_initialized = true;
    s_sntp_started_us = esp_timer_get_time();
    APP_LOGI(TAG,
             "SNTP request started: attempt=%u/%u primary=%s fallback=%s timeout=%d ms",
             (unsigned)s_sntp_attempt_count,
             (unsigned)SNTP_MAX_ATTEMPTS,
             primary_server,
             fallback_server,
             SNTP_ATTEMPT_TIMEOUT_MS);
    return ESP_OK;
}

static void sntp_task(void *arg)
{
    (void)arg;
    (void)watchdog_service_subscribe_current_task();
    for (;;) {
        watchdog_service_feed();
        EventBits_t bits = xEventGroupWaitBits(
            s_sntp_events, TIME_NET_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));

        if ((bits & TIME_NET_EVENT) != 0) {
            if (!wifi_manager_is_sta_connected()) {
                continue;
            }
            s_sntp_sync_applied = false;
            s_sntp_attempt_count = 0;
            s_sntp_retry_at_us = 0;
            s_sntp_retry_exhausted_logged = false;
            if (s_sntp_initialized) {
                esp_netif_sntp_deinit();
                s_sntp_initialized = false;
            }
            /* SNTP starts after the public probe; location waits for successful clock sync. */
            if (start_sntp_attempt() != ESP_OK) {
                schedule_sntp_retry("initialization");
            }
        }

        const int64_t now_us = esp_timer_get_time();
        if (!s_sntp_initialized && !s_sntp_sync_applied &&
            s_sntp_retry_at_us > 0 && now_us >= s_sntp_retry_at_us) {
            s_sntp_retry_at_us = 0;
            if (start_sntp_attempt() != ESP_OK) {
                schedule_sntp_retry("initialization");
            }
        }

        if (s_sntp_initialized && !s_sntp_sync_applied) {
            if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(100)) == ESP_OK) {
                apply_sntp_time();
            } else if (s_sntp_started_us > 0 &&
                       now_us - s_sntp_started_us >=
                           (int64_t)SNTP_ATTEMPT_TIMEOUT_MS * 1000) {
                esp_netif_sntp_deinit();
                s_sntp_initialized = false;
                s_sntp_started_us = 0;
                schedule_sntp_retry("timeout");
            }
        }
    }
}

static void location_task(void *arg)
{
    (void)arg;
    (void)watchdog_service_subscribe_current_task();
    int64_t next_refresh_us = 0;
    for (;;) {
        watchdog_service_feed();
        TickType_t wait_ticks = pdMS_TO_TICKS(500);
        if (next_refresh_us > 0) {
            const int64_t remaining_us = next_refresh_us - esp_timer_get_time();
            wait_ticks = remaining_us <= 0
                             ? 0
                             : pdMS_TO_TICKS((uint32_t)((remaining_us < 500000
                                                            ? remaining_us
                                                            : 500000) / 1000));
            if (wait_ticks == 0) wait_ticks = 1;
        }
        const EventBits_t bits = xEventGroupWaitBits(
            s_location_events, TIME_NET_EVENT, pdTRUE, pdFALSE, wait_ticks);
        watchdog_service_feed();
        const bool network_event = (bits & TIME_NET_EVENT) != 0;
        const bool periodic_refresh = next_refresh_us > 0 &&
                                      esp_timer_get_time() >= next_refresh_us;
        if (!network_event && !periodic_refresh) continue;

        if (!wifi_manager_is_sta_connected() ||
            !wifi_manager_is_internet_online()) {
            next_refresh_us = 0;
            continue;
        }
        bool cached_weather_released = false;
        if (network_event) {
            location_snapshot_t cached = {0};
            if (location_snapshot_get(&cached)) {
                /* Reuse the last known coordinates immediately while the
                 * fresh IP location request runs in this task. */
                app_weather_notify_network();
                cached_weather_released = true;
                APP_LOGI(TAG, "cached location available; weather refresh released");
            }
        }
        bool changed = false;
        if (refresh_location(&changed)) {
            (void)runtime_event_log_append(RUNTIME_EVENT_LOCATION_SUCCESS,
                                           changed ? 1 : 0, 0, 0);
            /* With cached coordinates the first weather request is already in
             * flight; only a changed location needs a second request. */
            if (!cached_weather_released || changed) app_weather_notify_network();
        } else {
            APP_LOGW(TAG, "no valid location; weather request postponed");
            (void)runtime_event_log_append(RUNTIME_EVENT_LOCATION_FAILURE, 0, 0, 0);
        }
        s_initial_location_settled = true;
        next_refresh_us = esp_timer_get_time() +
                          (int64_t)LOCATION_REFRESH_INTERVAL_MS * 1000;
    }
}

bool app_time_is_initial_location_settled(void)
{
    return s_initial_location_settled;
}

esp_err_t app_time_init(void)
{
    esp_err_t err = app_time_prepare_timezone();
    if (err != ESP_OK) return err;
    if (location_snapshot_init() != ESP_OK) return ESP_ERR_NO_MEM;

    s_sntp_events = xEventGroupCreate();
    s_location_events = xEventGroupCreate();
    if (s_sntp_events == NULL || s_location_events == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCoreWithCaps(sntp_task, "sntp_task", 3072, NULL, 4,
                    &s_sntp_task_handle, tskNO_AFFINITY,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    /* location_task 运行路径会写 NVS(location_snapshot_set -> nvs_set_blob),
     * 写入期间会禁用 SPI Cache。任务栈必须留在内部 RAM,否则 PSRAM 栈访问会触发
     * esp_task_stack_is_sane_cache_disabled() 断言崩溃。 */
    if (xTaskCreatePinnedToCore(location_task, "location_task", 4096, NULL, 3,
                                &s_location_task_handle, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (s_pending_http_time > 0) app_time_apply_http_time(s_pending_http_time);
    return ESP_OK;
}

void app_time_notify_network(void)
{
    if (s_sntp_events != NULL) xEventGroupSetBits(s_sntp_events, TIME_NET_EVENT);
}
