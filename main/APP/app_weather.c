#include "app_weather.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "app_log.h"
#include "http_memory_log.h"
#include "weather_snapshot.h"
#include "location_snapshot.h"
#include "runtime_event_log.h"
#include "time_snapshot.h"
#include "wifi_manager.h"
#include "watchdog_service.h"

#define WEATHER_NET_EVENT BIT0
#define WEATHER_REFRESH_INTERVAL_MS (30 * 60 * 1000)

static EventGroupHandle_t s_events;
static char s_body[2048];
static size_t s_len;
static bool s_body_overflow;
static TaskHandle_t s_weather_task_handle;
static volatile bool s_initial_weather_request_settled;
static const char *TAG = "app_weather";

static const char *weather_condition_name(int weather_code)
{
    if (weather_code == 0) return "clear";
    if (weather_code >= 1 && weather_code <= 3) return "cloudy";
    if (weather_code == 45 || weather_code == 48) return "fog";
    if (weather_code >= 51 && weather_code <= 57) return "drizzle";
    if (weather_code >= 61 && weather_code <= 67) return "rain";
    if (weather_code >= 80 && weather_code <= 82) return "showers";
    if (weather_code >= 71 && weather_code <= 77) return "snow";
    if (weather_code >= 85 && weather_code <= 86) return "snow_showers";
    if (weather_code == 95) return "thunder";
    if (weather_code >= 96) return "hail_thunder";
    return "unknown";
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    watchdog_service_feed();
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data != NULL &&
        s_len + event->data_len < sizeof(s_body)) {
        memcpy(s_body + s_len, event->data, event->data_len);
        s_len += event->data_len;
        s_body[s_len] = '\0';
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data != NULL) {
        s_body_overflow = true;
    }
    return ESP_OK;
}

/* Open-Meteo 的 daily 段是"每个字段一个数组"，下标 0=今天、1=明天。
   长度或类型不对就整体放弃预报：宁可没有预报，也不能只填一半让充电策略拿
   半个数据去算。这一段只是附加项，缺了不影响既有的温度/天气显示链路。 */
static void parse_daily_forecast(const cJSON *root, weather_snapshot_t *snapshot)
{
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (!cJSON_IsObject(daily)) return;
    const cJSON *tmin = cJSON_GetObjectItem(daily, "temperature_2m_min");
    const cJSON *tmax = cJSON_GetObjectItem(daily, "temperature_2m_max");
    const cJSON *precip = cJSON_GetObjectItem(daily, "precipitation_probability_max");
    if (!cJSON_IsArray(tmin) || !cJSON_IsArray(tmax) || !cJSON_IsArray(precip)) return;
    if (cJSON_GetArraySize(tmin) < 2 || cJSON_GetArraySize(tmax) < 2 ||
        cJSON_GetArraySize(precip) < 2) {
        return;
    }
    const cJSON *tmin0 = cJSON_GetArrayItem(tmin, 0);
    const cJSON *tmin1 = cJSON_GetArrayItem(tmin, 1);
    const cJSON *tmax0 = cJSON_GetArrayItem(tmax, 0);
    const cJSON *tmax1 = cJSON_GetArrayItem(tmax, 1);
    const cJSON *p0 = cJSON_GetArrayItem(precip, 0);
    const cJSON *p1 = cJSON_GetArrayItem(precip, 1);
    if (!cJSON_IsNumber(tmin0) || !cJSON_IsNumber(tmin1) || !cJSON_IsNumber(tmax0) ||
        !cJSON_IsNumber(tmax1) || !cJSON_IsNumber(p0) || !cJSON_IsNumber(p1)) {
        return;
    }
    snapshot->today_temp_min_c = (int8_t)tmin0->valueint;
    snapshot->today_temp_max_c = (int8_t)tmax0->valueint;
    snapshot->today_precip_prob = (uint8_t)p0->valueint;
    snapshot->tomorrow_temp_min_c = (int8_t)tmin1->valueint;
    snapshot->tomorrow_temp_max_c = (int8_t)tmax1->valueint;
    snapshot->tomorrow_precip_prob = (uint8_t)p1->valueint;
    snapshot->forecast_valid = true;
}

static void refresh_weather(void)
{
    s_len = 0;
    s_body[0] = '\0';
    s_body_overflow = false;

    location_snapshot_t location = {0};
    /* 定位失败时不使用伪造坐标，保留上一次有效网络天气。 */
    if (!location_snapshot_get(&location)) {
        APP_LOGW(TAG, "skip weather request: no valid location");
        return;
    }

    char url[512];
    /* daily 段是给充电策略用的：低温掉电快、雨天阻力大，明天气温和降水概率
     * 直接决定"今晚要不要补电"。显式写 forecast_days=2 —— 默认 7 天，多拉的
     * 那五天只会让响应白白变大。 */
    int url_len = snprintf(
        url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f"
        "&current=temperature_2m,weather_code"
        "&daily=temperature_2m_min,temperature_2m_max,precipitation_probability_max"
        "&forecast_days=2&timezone=auto",
        location.latitude, location.longitude);
    if (url_len < 0 || (size_t)url_len >= sizeof(url)) {
        APP_LOGE(TAG, "weather URL buffer is too small");
        return;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return;
    }

    http_memory_log("before", "weather");
    watchdog_service_set_current_task_blocking(true);
    esp_err_t request_err = esp_http_client_perform(client);
    watchdog_service_set_current_task_blocking(false);
    int status = esp_http_client_get_status_code(client);
    if (request_err != ESP_OK) {
        APP_LOG_ERR(TAG, request_err, "weather request failed, status=%d", status);
        runtime_event_log_append(RUNTIME_EVENT_WEATHER_FAILURE, status, request_err, 0);
    } else if (s_body_overflow) {
        APP_LOGE(TAG, "weather response overflow, status=%d", status);
        runtime_event_log_append(RUNTIME_EVENT_WEATHER_FAILURE, status, 1, 0);
    } else if (status != 200) {
        APP_LOGW(TAG, "weather request returned HTTP %d", status);
        runtime_event_log_append(RUNTIME_EVENT_WEATHER_FAILURE, status, 2, 0);
    } else {
        cJSON *root = cJSON_Parse(s_body);
        cJSON *current = root != NULL ? cJSON_GetObjectItem(root, "current") : NULL;

        if (root == NULL) {
            APP_LOGW(TAG, "weather response JSON parse failed");
        } else if (current != NULL) {
            weather_snapshot_t snapshot = {0};
            cJSON *value;
            bool has_temperature = false;
            bool has_weather_code = false;

            value = cJSON_GetObjectItem(current, "temperature_2m");
            if (value != NULL && cJSON_IsNumber(value)) {
                snapshot.temperature_c = (float)value->valuedouble;
                has_temperature = true;
            }

            value = cJSON_GetObjectItem(current, "weather_code");
            if (value != NULL && cJSON_IsNumber(value)) {
                snapshot.weather_code = value->valueint;
                has_weather_code = true;
            }

            parse_daily_forecast(root, &snapshot);

            /* 只有完整响应才能覆盖上一份网络天气。 */
            if (has_temperature && has_weather_code) {
                snapshot.valid = true;
                snapshot.temperature_valid = true;
                snapshot.temperature_source = WEATHER_TEMP_SOURCE_NETWORK;
                snapshot.updated_at = time(NULL);
                weather_snapshot_set(&snapshot);
                APP_LOGI(TAG,
                         "weather code=%d condition=%s temperature_x10=%d forecast=%d "
                         "today=%d~%d precip=%u%% tomorrow=%d~%d precip=%u%%",
                         snapshot.weather_code,
                         weather_condition_name(snapshot.weather_code),
                         (int)(snapshot.temperature_c * 10.0f),
                         (int)snapshot.forecast_valid,
                         (int)snapshot.today_temp_min_c, (int)snapshot.today_temp_max_c,
                         (unsigned)snapshot.today_precip_prob,
                         (int)snapshot.tomorrow_temp_min_c, (int)snapshot.tomorrow_temp_max_c,
                         (unsigned)snapshot.tomorrow_precip_prob);
                runtime_event_log_append(RUNTIME_EVENT_WEATHER_SUCCESS,
                                         snapshot.weather_code,
                                         (int32_t)(snapshot.temperature_c * 10.0f),
                                         0);
            } else {
                APP_LOGW(TAG, "weather JSON missing required current fields");
                runtime_event_log_append(RUNTIME_EVENT_WEATHER_FAILURE, status, 3, 0);
            }
        } else {
            APP_LOGW(TAG, "weather JSON missing current object");
            runtime_event_log_append(RUNTIME_EVENT_WEATHER_FAILURE, status, 4, 0);
        }
        cJSON_Delete(root);
    }

    esp_http_client_cleanup(client);
    http_memory_log("after", "weather");
}

static void weather_task(void *arg)
{
    (void)arg;
    (void)watchdog_service_subscribe_current_task();
    int64_t next_refresh_us = esp_timer_get_time() +
                              (int64_t)WEATHER_REFRESH_INTERVAL_MS * 1000;
    for (;;) {
        watchdog_service_feed();
        TickType_t wait_ticks = pdMS_TO_TICKS(500);
        const int64_t remaining_us = next_refresh_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            wait_ticks = 1;
        } else if (remaining_us < 500000) {
            wait_ticks = pdMS_TO_TICKS((uint32_t)(remaining_us / 1000));
            if (wait_ticks == 0) wait_ticks = 1;
        }
        EventBits_t bits = xEventGroupWaitBits(
            s_events, WEATHER_NET_EVENT, pdTRUE, pdFALSE,
            wait_ticks);
        watchdog_service_feed();
        const bool periodic_refresh = esp_timer_get_time() >= next_refresh_us;
        if ((bits & WEATHER_NET_EVENT) == 0 && !periodic_refresh) continue;
        if (wifi_manager_is_sta_connected() && wifi_manager_is_internet_online() &&
            time_snapshot_is_ready()) {
            refresh_weather();
            s_initial_weather_request_settled = true;
        }
        next_refresh_us = esp_timer_get_time() +
                          (int64_t)WEATHER_REFRESH_INTERVAL_MS * 1000;
    }
}

esp_err_t app_weather_init(void)
{
    if (weather_snapshot_init() != ESP_OK) return ESP_ERR_NO_MEM;
    s_events = xEventGroupCreate();
    if (s_events == NULL) return ESP_ERR_NO_MEM;
    /* 天气刷新只做 HTTPS 请求和内存快照，从不写 NVS，所以任务栈可以放在 PSRAM。
     * 天气/位置/激活任务与语音管道并发启动，把常驻任务占的内部 RAM 降到最低，
     * ESP-SR 创建 WakeNet 时才能稳定拿到连续块。 */
    return xTaskCreatePinnedToCoreWithCaps(weather_task, "weather_task", 6144, NULL, 3,
                                           &s_weather_task_handle, 0,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

TaskHandle_t app_weather_get_task_handle(void)
{
    return s_weather_task_handle;
}

void app_weather_notify_network(void)
{
    if (s_events != NULL) xEventGroupSetBits(s_events, WEATHER_NET_EVENT);
}

bool app_weather_is_initial_request_settled(void)
{
    return s_initial_weather_request_settled;
}
