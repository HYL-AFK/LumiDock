#include "tariff_service.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "http_memory_log.h"
#include "time_snapshot.h"
#include "watchdog_service.h"
#include "wifi_manager.h"

static const char *const TAG = "tariff";

#define TARIFF_NET_EVENT BIT0
/* 电价本身是日前市场，一天只定一次，每小时刷一次已经很勤了。 */
#define TARIFF_REFRESH_INTERVAL_MS (60 * 60 * 1000)
#define TARIFF_HTTP_TIMEOUT_MS 8000
/* awattar 实测返回约 1.5 KB（19 条），留一倍余量。放 PSRAM，不吃内部 RAM。 */
#define TARIFF_BODY_BYTES 4096U
#define TARIFF_URL "https://api.awattar.de/v1/marketdata"

/* 德国到户电价里与市场价无关的那一坨（电网费、税费、零售商加价）。
 * 接口给的是 EPEX Spot 批发电价，不加这个，"能省多少钱"会差出二十多 ct/kWh。 */
#define TARIFF_DEFAULT_SURCHARGE_CT 20.0f

/* 本地兜底默认值：德国常见的夜间谷电。充电配置服务读回 NVS 后会覆盖。 */
#define TARIFF_FALLBACK_OFFPEAK_START 22U
#define TARIFF_FALLBACK_OFFPEAK_END 6U
#define TARIFF_FALLBACK_OFFPEAK_CT 20.0f
#define TARIFF_FALLBACK_PEAK_CT 35.0f

#define TARIFF_SECONDS_PER_HOUR 3600LL

/* 只在电价任务里写，其它任务只读快照副本，所以不加锁：读到的最大代价是
 * 一份跨刷新边界的混合数据，而电价本身是小时级的慢变量。 */
static tariff_snapshot_t s_snapshot;
static EventGroupHandle_t s_events;
static TaskHandle_t s_task_handle;

/* 响应体放 PSRAM。任务栈也在 PSRAM，两者都不占内部 RAM。 */
static char *s_body;
static size_t s_body_len;
static bool s_body_overflow;

static float s_surcharge_ct = TARIFF_DEFAULT_SURCHARGE_CT;
static uint8_t s_offpeak_start = TARIFF_FALLBACK_OFFPEAK_START;
static uint8_t s_offpeak_end = TARIFF_FALLBACK_OFFPEAK_END;
static float s_offpeak_ct = TARIFF_FALLBACK_OFFPEAK_CT;
static float s_peak_ct = TARIFF_FALLBACK_PEAK_CT;

static bool hour_in_offpeak(uint8_t hour)
{
    /* 跨零点的区间（22 -> 6）是最常见的形式，必须当成两段来看。 */
    if (s_offpeak_start <= s_offpeak_end)
        return hour >= s_offpeak_start && hour < s_offpeak_end;
    return hour >= s_offpeak_start || hour < s_offpeak_end;
}

/* 接口拉不到时的兜底：按本地峰谷时段铺满 24 小时。
 * 宁可给一份"大致对"的曲线，也不要让充电策略因为缺数据而彻底哑掉。 */
static void apply_fallback(void)
{
    tariff_snapshot_t snap = {0};
    const time_t now = time(NULL);
    /* 对齐到整点，和接口的槽位口径保持一致。 */
    time_t base = now - (now % (time_t)TARIFF_SECONDS_PER_HOUR);
    struct tm local = {0};
    localtime_r(&base, &local);
    /* 从当前整点往前退到本地零点，这样槽位下标和"第几小时"一一对应。 */
    base -= (time_t)local.tm_hour * (time_t)TARIFF_SECONDS_PER_HOUR;

    snap.from_network = false;
    snap.is_wholesale = false; /* 本地配置填的就是到户价，不再叠加附加费 */
    snap.surcharge_ct = 0.0f;
    snap.slot_count = TARIFF_MAX_SLOTS;
    for (uint8_t i = 0U; i < TARIFF_MAX_SLOTS; ++i) {
        snap.slots[i].start_ts = (int64_t)base + (int64_t)i * TARIFF_SECONDS_PER_HOUR;
        snap.slots[i].price_ct = hour_in_offpeak(i) ? s_offpeak_ct : s_peak_ct;
    }
    snap.valid = true;
    snap.updated_at_ts = (int64_t)now;

    float sum = 0.0f, min_ct = snap.slots[0].price_ct, max_ct = snap.slots[0].price_ct;
    int64_t cheapest_ts = snap.slots[0].start_ts;
    for (uint8_t i = 0U; i < snap.slot_count; ++i) {
        const float p = snap.slots[i].price_ct;
        sum += p;
        if (p < min_ct) { min_ct = p; cheapest_ts = snap.slots[i].start_ts; }
        if (p > max_ct) max_ct = p;
    }
    snap.avg_ct = sum / (float)snap.slot_count;
    snap.min_ct = min_ct;
    snap.max_ct = max_ct;
    snap.cheapest_start_ts = cheapest_ts;
    /* 兜底数据的"当前价"就是本地时段价，按本地时钟取。 */
    localtime_r(&now, &local);
    snap.current_ct = hour_in_offpeak((uint8_t)local.tm_hour) ? s_offpeak_ct : s_peak_ct;

    s_snapshot = snap;
    ESP_LOGW(TAG, "电价回退到本地峰谷：谷 %02u:00-%02u:00 %.1f ct，峰 %.1f ct",
             (unsigned)s_offpeak_start, (unsigned)s_offpeak_end,
             (double)s_offpeak_ct, (double)s_peak_ct);
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    watchdog_service_feed();
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data != NULL) {
        if (s_body != NULL && s_body_len + (size_t)event->data_len < TARIFF_BODY_BYTES) {
            memcpy(s_body + s_body_len, event->data, (size_t)event->data_len);
            s_body_len += (size_t)event->data_len;
            s_body[s_body_len] = '\0';
        } else {
            s_body_overflow = true;
        }
    }
    return ESP_OK;
}

/* 把接口响应翻译成快照。返回 false 时调用方会用兜底数据。 */
static bool parse_marketdata(void)
{
    cJSON *root = cJSON_Parse(s_body);
    if (root == NULL) {
        ESP_LOGW(TAG, "电价响应 JSON 解析失败");
        return false;
    }
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(data)) {
        ESP_LOGW(TAG, "电价响应缺少 data 数组");
        cJSON_Delete(root);
        return false;
    }

    tariff_snapshot_t snap = {0};
    snap.from_network = true;
    snap.is_wholesale = true;
    snap.surcharge_ct = s_surcharge_ct;
    snap.slot_count = 0U;

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, data) {
        if (snap.slot_count >= TARIFF_MAX_SLOTS) break;
        const cJSON *start = cJSON_GetObjectItem(item, "start_timestamp");
        const cJSON *price = cJSON_GetObjectItem(item, "marketprice");
        if (!cJSON_IsNumber(start) || !cJSON_IsNumber(price)) continue;
        /* 毫秒时间戳（1.79e12）超出 int 范围，valueint 会截断，必须读
         * valuedouble —— double 有 53 位尾数，装到 9e15 仍然精确。 */
        const int64_t start_ts = (int64_t)(start->valuedouble / 1000.0);
        /* 接口单位是 EUR/MWh：1 EUR/MWh = 0.1 ct/kWh。 */
        const float wholesale_ct = (float)(price->valuedouble / 10.0);
        snap.slots[snap.slot_count].start_ts = start_ts;
        snap.slots[snap.slot_count].price_ct = wholesale_ct + s_surcharge_ct;
        ++snap.slot_count;
    }
    cJSON_Delete(root);

    if (snap.slot_count == 0U) {
        ESP_LOGW(TAG, "电价响应没有任何可用槽位");
        return false;
    }

    /* 派生值一次算好。最低价同时记下标，充电策略要拿它排计划。 */
    float sum = 0.0f;
    float min_ct = snap.slots[0].price_ct, max_ct = snap.slots[0].price_ct;
    int64_t cheapest_ts = snap.slots[0].start_ts;
    for (uint8_t i = 0U; i < snap.slot_count; ++i) {
        const float p = snap.slots[i].price_ct;
        sum += p;
        if (p < min_ct) { min_ct = p; cheapest_ts = snap.slots[i].start_ts; }
        if (p > max_ct) max_ct = p;
    }
    snap.avg_ct = sum / (float)snap.slot_count;
    snap.min_ct = min_ct;
    snap.max_ct = max_ct;
    snap.cheapest_start_ts = cheapest_ts;

    const time_t now = time(NULL);
    snap.updated_at_ts = (int64_t)now;
    snap.current_ct = snap.slots[0].price_ct;
    for (uint8_t i = 0U; i < snap.slot_count; ++i) {
        if (snap.slots[i].start_ts <= (int64_t)now &&
            (int64_t)now < snap.slots[i].start_ts + TARIFF_SECONDS_PER_HOUR) {
            snap.current_ct = snap.slots[i].price_ct;
            break;
        }
    }
    snap.valid = true;
    s_snapshot = snap;

    ESP_LOGI(TAG, "电价已更新：%u 档 当前 %.2f 均价 %.2f 最低 %.2f 最高 %.2f ct/kWh（含固定附加 %.1f）",
             (unsigned)snap.slot_count, (double)snap.current_ct, (double)snap.avg_ct,
             (double)snap.min_ct, (double)snap.max_ct, (double)s_surcharge_ct);
    return true;
}

static void refresh_tariff(void)
{
    if (s_body == NULL) {
        s_body = heap_caps_malloc(TARIFF_BODY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_body == NULL) {
            ESP_LOGW(TAG, "电价响应缓冲分配失败");
            return;
        }
    }
    s_body_len = 0U;
    s_body[0] = '\0';
    s_body_overflow = false;

    const esp_http_client_config_t cfg = {
        .url = TARIFF_URL,
        .event_handler = http_event,
        .timeout_ms = TARIFF_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGW(TAG, "电价 HTTP client 创建失败");
        apply_fallback();
        return;
    }

    http_memory_log("before", "tariff");
    watchdog_service_set_current_task_blocking(true);
    const esp_err_t err = esp_http_client_perform(client);
    watchdog_service_set_current_task_blocking(false);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    http_memory_log("after", "tariff");

    if (err != ESP_OK || status != 200 || s_body_overflow) {
        ESP_LOGW(TAG, "电价请求失败 err=%s status=%d overflow=%d，改用本地峰谷",
                 esp_err_to_name(err), status, (int)s_body_overflow);
        apply_fallback();
        return;
    }
    if (!parse_marketdata()) {
        apply_fallback();
    }
}

static void tariff_task(void *arg)
{
    (void)arg;
    (void)watchdog_service_subscribe_current_task();
    int64_t next_refresh_us = esp_timer_get_time() + (int64_t)TARIFF_REFRESH_INTERVAL_MS * 1000;
    for (;;) {
        watchdog_service_feed();
        TickType_t wait_ticks = pdMS_TO_TICKS(1000);
        const int64_t remaining_us = next_refresh_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            wait_ticks = 1;
        } else if (remaining_us < 1000000) {
            wait_ticks = pdMS_TO_TICKS((uint32_t)(remaining_us / 1000));
            if (wait_ticks == 0) wait_ticks = 1;
        }
        const EventBits_t bits = s_events != NULL
                                     ? xEventGroupWaitBits(s_events, TARIFF_NET_EVENT, pdTRUE,
                                                           pdFALSE, wait_ticks)
                                     : 0;
        watchdog_service_feed();
        const bool periodic = esp_timer_get_time() >= next_refresh_us;
        if ((bits & TARIFF_NET_EVENT) == 0 && !periodic) continue;

        /* 电价要联网，也要知道"现在几点"才能挑出当前档位；时钟没同步时
         * 拉回来也定位不了 current，所以两个条件都要满足。 */
        if (wifi_manager_is_sta_connected() && wifi_manager_is_internet_online() &&
            time_snapshot_is_ready()) {
            refresh_tariff();
        }
        next_refresh_us = esp_timer_get_time() + (int64_t)TARIFF_REFRESH_INTERVAL_MS * 1000;
    }
}

esp_err_t tariff_service_init(void)
{
    /* 先铺一份兜底数据：开机到第一次成功拉取之间（可能好几分钟）充电策略
     * 也得能答话，不能因为"还没联网"就什么都不说。 */
    apply_fallback();
    s_events = xEventGroupCreate();
    if (s_events == NULL) return ESP_ERR_NO_MEM;
    /* 任务只做 HTTPS 和内存快照，从不写 NVS，所以栈可以放 PSRAM：天气和语音
     * 管道并发起来时，把常驻任务占的内部 RAM 压到最低。 */
    return xTaskCreatePinnedToCoreWithCaps(tariff_task, "tariff_task", 6144, NULL, 3,
                                           &s_task_handle, 0,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

void tariff_service_notify_network(void)
{
    if (s_events != NULL) xEventGroupSetBits(s_events, TARIFF_NET_EVENT);
}

bool tariff_service_get(tariff_snapshot_t *out)
{
    if (out == NULL || !s_snapshot.valid) return false;
    *out = s_snapshot;
    return true;
}

void tariff_service_set_fallback(uint8_t offpeak_start_hour, uint8_t offpeak_end_hour,
                                 float offpeak_ct, float peak_ct)
{
    if (offpeak_start_hour > 23U || offpeak_end_hour > 23U) return;
    s_offpeak_start = offpeak_start_hour;
    s_offpeak_end = offpeak_end_hour;
    s_offpeak_ct = offpeak_ct;
    s_peak_ct = peak_ct;
    /* 已经拿过接口数据就别用配置把它顶掉；只有当前正跑兜底时才立即重铺。 */
    if (!s_snapshot.valid || !s_snapshot.from_network) apply_fallback();
}

bool tariff_service_find_cheapest(const tariff_snapshot_t *snap, uint8_t hours,
                                  int64_t *start_ts, float *avg_ct)
{
    if (snap == NULL || !snap->valid || hours == 0U || hours > snap->slot_count)
        return false;
    /* 槽位最多 24 个，O(n^2) 完全够用，不值得为它引滑窗。 */
    float best_avg = 0.0f;
    int best = -1;
    for (uint8_t i = 0U; (uint8_t)(i + hours) <= snap->slot_count; ++i) {
        float sum = 0.0f;
        for (uint8_t k = 0U; k < hours; ++k) sum += snap->slots[i + k].price_ct;
        const float avg = sum / (float)hours;
        if (best < 0 || avg < best_avg) { best = (int)i; best_avg = avg; }
    }
    if (best < 0) return false;
    if (start_ts != NULL) *start_ts = snap->slots[best].start_ts;
    if (avg_ct != NULL) *avg_ct = best_avg;
    return true;
}
