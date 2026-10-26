#include "device_data.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "app_log.h"

#define TAG "device_data"

#define DEVICE_DATA_VERSION 1U
#define DEVICE_DATA_FACTORY_MAGIC 0x46414354u /* "FACT" */
#define DEVICE_DATA_SYSTEM_MAGIC 0x53595354u  /* "SYST" */

/* 两个独立命名空间：生产数据自始至终只写一次，和每五分钟都要刷一次的系统数据
 * 混在同一个名字下，会让"恢复出厂只清系统数据"变成一件做不到的事。 */
#define DEVICE_DATA_FACTORY_NS "dev_factory"
#define DEVICE_DATA_FACTORY_KEY "record"
#define DEVICE_DATA_SYSTEM_NS "dev_system"
#define DEVICE_DATA_SYSTEM_KEY "record"

/* 早于 2020-01-01 的 time() 只是从 1970 起算的假值，说明 SNTP 还没同步。 */
#define DEVICE_DATA_TIME_VALID_MIN 1577836800

/* 落盘周期。
 *
 * 不再缩短，是因为 NVS 每次写入都会禁用 SPI Cache 十几毫秒，这期间 PSRAM 上的
 * 任务全部停摆（音频播放、网络收发都挂在上面）。运行时长和流量是内部记录用途，
 * 掉电最多丢一个周期的精度，换的是每分钟少一次周期性停顿。 */
#define DEVICE_DATA_FLUSH_PERIOD_US (5LL * 60LL * 1000LL * 1000LL)

_Static_assert(sizeof(device_data_factory_t) == 176,
               "device factory record layout changed");
_Static_assert(sizeof(device_data_system_t) == 100,
               "device system record layout changed");

static device_data_factory_t s_factory;
static device_data_system_t s_system;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_flush_timer;
static bool s_initialized;
static int64_t s_uptime_mark_us;

/* 流量走独立的自旋锁，不和 s_lock 共用。
 *
 * s_lock 在落盘时会被 NVS 写入占住十几毫秒，而流量的调用点在 HTTP/WSS 的读回调
 * 里——如果它们也去等 s_lock，就等于把落盘的停顿原样转嫁给网络任务，正是
 * runtime_event_log 当初要避开的那个坑。 */
static portMUX_TYPE s_traffic_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_traffic_total_rx;
static uint64_t s_traffic_total_tx;
static uint64_t s_traffic_boot_rx;
static uint64_t s_traffic_boot_tx;

static void device_data_flush_timer_cb(void *arg);

/* ------------------------------------------------------------------ 校验 */

static uint32_t crc32_of(const void *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t index = 0U; index < length; ++index) {
        crc ^= bytes[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1) ^
                  (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

/* crc32 字段自己不参与校验，否则算不出来。 */
static uint32_t factory_checksum(void)
{
    return crc32_of(&s_factory, offsetof(device_data_factory_t, crc32));
}

static uint32_t system_checksum(void)
{
    return crc32_of(&s_system, offsetof(device_data_system_t, crc32));
}

static bool factory_usable(void)
{
    return s_factory.magic == DEVICE_DATA_FACTORY_MAGIC &&
           s_factory.version == DEVICE_DATA_VERSION &&
           s_factory.crc32 == factory_checksum();
}

static bool system_usable(void)
{
    return s_system.magic == DEVICE_DATA_SYSTEM_MAGIC &&
           s_system.version == DEVICE_DATA_VERSION &&
           s_system.crc32 == system_checksum();
}

/* ------------------------------------------------------------------ NVS */

static esp_err_t save_blob(const char *name_space, const char *key,
                           const void *data, size_t length)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(name_space, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, key, data, length);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static bool load_blob(const char *name_space, const char *key,
                      void *data, size_t length)
{
    nvs_handle_t handle;
    if (nvs_open(name_space, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t size = length;
    const bool ok = (nvs_get_blob(handle, key, data, &size) == ESP_OK &&
                     size == length);
    nvs_close(handle);
    return ok;
}

static esp_err_t save_factory(void)
{
    s_factory.crc32 = factory_checksum();
    const esp_err_t err = save_blob(DEVICE_DATA_FACTORY_NS, DEVICE_DATA_FACTORY_KEY,
                                    &s_factory, sizeof(s_factory));
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "生产数据落盘失败");
    }
    return err;
}

static esp_err_t save_system(void)
{
    s_system.crc32 = system_checksum();
    const esp_err_t err = save_blob(DEVICE_DATA_SYSTEM_NS, DEVICE_DATA_SYSTEM_KEY,
                                    &s_system, sizeof(s_system));
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "系统数据落盘失败");
    }
    return err;
}

/* ------------------------------------------------------------------ 采集 */

static const char *chip_model_name(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    default:           return "UNKNOWN";
    }
}

/* 只在首次开机调用一次：把这块板子的身份固化下来。 */
static void collect_factory(void)
{
    memset(&s_factory, 0, sizeof(s_factory));
    s_factory.magic = DEVICE_DATA_FACTORY_MAGIC;
    s_factory.version = DEVICE_DATA_VERSION;

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    strlcpy(s_factory.chip_model, chip_model_name(chip.model), sizeof(s_factory.chip_model));
    s_factory.chip_revision = (uint8_t)chip.revision;
    s_factory.core_count = (uint8_t)chip.cores;

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        memcpy(s_factory.mac, mac, sizeof(s_factory.mac));
    }
    /* 序列号由 MAC 派生：同一颗芯片永远得到同一个号，不依赖产线工装就能先跑起来。
     * 将来接了产线烧号，把这里换成读工装写入的值即可，字段本身不用动。 */
    snprintf(s_factory.serial, sizeof(s_factory.serial),
             "LD-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint32_t flash_size = 0U;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        s_factory.flash_size_bytes = flash_size;
    }
    s_factory.psram_size_bytes = esp_psram_is_initialized()
                                     ? (uint32_t)esp_psram_get_size()
                                     : 0U;

    const esp_app_desc_t *app = esp_app_get_description();
    if (app != NULL) {
        strlcpy(s_factory.fw_version, app->version, sizeof(s_factory.fw_version));
        strlcpy(s_factory.fw_build_date, app->date, sizeof(s_factory.fw_build_date));
        strlcpy(s_factory.fw_build_time, app->time, sizeof(s_factory.fw_build_time));
        strlcpy(s_factory.idf_version, app->idf_ver, sizeof(s_factory.idf_version));
    }
    s_factory.board_revision = 1U;
    /* manufacture_ts 留在 0：首次开机时 SNTP 多半还没同步，由 backfill_time()
     * 在第一次拿到有效时间后补上。 */
}

static bool is_abnormal_reset(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ 累积 */

static void update_uptime(void)
{
    const int64_t now = esp_timer_get_time();
    if (now > s_uptime_mark_us) {
        s_system.total_uptime_s +=
            (uint64_t)((now - s_uptime_mark_us) / 1000000LL);
        s_uptime_mark_us = now;
    }
}

/* 开机时若时间还没同步，先留 0；等 SNTP 落地后在这里补。
 * 补出来的"首次开机时刻"会比真实开机晚一点点，误差就是开机到同步的那几秒。 */
static void backfill_time(void)
{
    const time_t now = time(NULL);
    if (now < DEVICE_DATA_TIME_VALID_MIN) return;
    const uint64_t ts = (uint64_t)now;

    if (s_system.first_boot_ts == 0U) s_system.first_boot_ts = ts;
    if (s_system.last_boot_ts == 0U) s_system.last_boot_ts = ts;
    if (s_factory.manufacture_ts == 0U) {
        s_factory.manufacture_ts = ts;
        (void)save_factory();
    }
}

static void snapshot_traffic(void)
{
    portENTER_CRITICAL(&s_traffic_mux);
    const uint64_t total_rx = s_traffic_total_rx;
    const uint64_t total_tx = s_traffic_total_tx;
    const uint64_t boot_rx = s_traffic_boot_rx;
    const uint64_t boot_tx = s_traffic_boot_tx;
    portEXIT_CRITICAL(&s_traffic_mux);

    s_system.traffic_total_rx = total_rx;
    s_system.traffic_total_tx = total_tx;
    s_system.traffic_boot_rx = boot_rx;
    s_system.traffic_boot_tx = boot_tx;
}

/* 调用者必须已持有 s_lock。 */
static esp_err_t flush_locked(void)
{
    update_uptime();
    backfill_time();
    snapshot_traffic();
    return save_system();
}

static void device_data_flush_timer_cb(void *arg)
{
    (void)arg;
    if (!s_initialized) return;

    /* 用超时拿锁：万一 NVS 正忙，宁可跳过这一轮，也不要在定时器任务里排队，
     * 那会把 esp_timer 上其它定时器一起拖住。 */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    (void)flush_locked();
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ 接口 */

esp_err_t device_data_init(void)
{
    if (s_initialized) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    if (!load_blob(DEVICE_DATA_FACTORY_NS, DEVICE_DATA_FACTORY_KEY,
                   &s_factory, sizeof(s_factory)) ||
        !factory_usable()) {
        collect_factory();
        (void)save_factory();
        APP_LOGI(TAG, "生产数据已生成：%s %s 序列号=%s flash=%lu psram=%lu",
                 s_factory.chip_model, s_factory.fw_version, s_factory.serial,
                 (unsigned long)s_factory.flash_size_bytes,
                 (unsigned long)s_factory.psram_size_bytes);
    }

    if (!load_blob(DEVICE_DATA_SYSTEM_NS, DEVICE_DATA_SYSTEM_KEY,
                   &s_system, sizeof(s_system)) ||
        !system_usable()) {
        memset(&s_system, 0, sizeof(s_system));
        s_system.magic = DEVICE_DATA_SYSTEM_MAGIC;
        s_system.version = DEVICE_DATA_VERSION;
    }

    /* 开机计数与复位归因。首开那次不算异常——上电本来就是这个原因。 */
    const esp_reset_reason_t reason = esp_reset_reason();
    s_system.boot_count++;
    s_system.last_reset_reason = (uint32_t)reason;
    if (s_system.boot_count > 1U && is_abnormal_reset(reason)) {
        s_system.abnormal_resets++;
    }

    const time_t now = time(NULL);
    if (now >= DEVICE_DATA_TIME_VALID_MIN) {
        const uint64_t ts = (uint64_t)now;
        s_system.last_boot_ts = ts;
        if (s_system.first_boot_ts == 0U) s_system.first_boot_ts = ts;
        if (s_factory.manufacture_ts == 0U) {
            s_factory.manufacture_ts = ts;
            (void)save_factory();
        }
    }

    /* 流量基线接上关机前累计的值，本次开机计数从零开始。 */
    s_traffic_total_rx = s_system.traffic_total_rx;
    s_traffic_total_tx = s_system.traffic_total_tx;
    s_traffic_boot_rx = 0U;
    s_traffic_boot_tx = 0U;
    s_uptime_mark_us = esp_timer_get_time();

    /* 开机这条立刻落地：万一此刻就掉电，至少次数已经记上。 */
    (void)save_system();
    s_initialized = true;

    esp_timer_create_args_t timer_args = {
        .callback = device_data_flush_timer_cb,
        .name = "devdata_flush",
    };
    if (esp_timer_create(&timer_args, &s_flush_timer) == ESP_OK) {
        esp_timer_start_periodic(s_flush_timer, DEVICE_DATA_FLUSH_PERIOD_US);
    }

    APP_LOGI(TAG, "设备数据就绪：第 %lu 次开机，上次复位=%lu，累计运行 %llu s",
             (unsigned long)s_system.boot_count,
             (unsigned long)s_system.last_reset_reason,
             (unsigned long long)s_system.total_uptime_s);
    return ESP_OK;
}

const device_data_factory_t *device_data_factory(void)
{
    return s_initialized ? &s_factory : NULL;
}

const device_data_system_t *device_data_system(void)
{
    return s_initialized ? &s_system : NULL;
}

void device_data_traffic_get(device_data_traffic_t *out)
{
    if (out == NULL) return;

    portENTER_CRITICAL(&s_traffic_mux);
    out->total_rx = s_traffic_total_rx;
    out->total_tx = s_traffic_total_tx;
    out->boot_rx = s_traffic_boot_rx;
    out->boot_tx = s_traffic_boot_tx;
    portEXIT_CRITICAL(&s_traffic_mux);
}

void device_data_mark_activated(void)
{
    if (!s_initialized) return;

    /* 刻意不在这里落盘。
     *
     * 唯一的调用方是激活任务，而它的栈在 PSRAM 上（内部 RAM 要留给 ESP-SR），
     * NVS 写入期间会禁用 SPI Cache——在 PSRAM 栈上执行写 NVS 的代码会直接触发
     * 断言崩溃。所以时刻先记在内存，由周期 flush 带出去。真在落盘前掉电，丢的
     * 也只是一个时间戳：是否已激活由 xiaozhi 命名空间里的 client_id 决定。 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool newly_marked = false;
    if (s_system.activated == 0U) {
        s_system.activated = 1U;
        newly_marked = true;
        const time_t now = time(NULL);
        if (now >= DEVICE_DATA_TIME_VALID_MIN) {
            s_system.activated_ts = (uint64_t)now;
        }
    }
    const uint64_t activated_ts = s_system.activated_ts;
    xSemaphoreGive(s_lock);

    if (newly_marked) {
        APP_LOGI(TAG, "激活时刻已记录：%llu（随下一次周期落盘）",
                 (unsigned long long)activated_ts);
    }
}

void device_data_traffic_add_rx(size_t bytes)
{
    if (!s_initialized || bytes == 0U) return;

    portENTER_CRITICAL(&s_traffic_mux);
    s_traffic_total_rx += (uint64_t)bytes;
    s_traffic_boot_rx += (uint64_t)bytes;
    portEXIT_CRITICAL(&s_traffic_mux);
}

void device_data_traffic_add_tx(size_t bytes)
{
    if (!s_initialized || bytes == 0U) return;

    portENTER_CRITICAL(&s_traffic_mux);
    s_traffic_total_tx += (uint64_t)bytes;
    s_traffic_boot_tx += (uint64_t)bytes;
    portEXIT_CRITICAL(&s_traffic_mux);
}

esp_err_t device_data_flush(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t err = flush_locked();
    xSemaphoreGive(s_lock);
    return err;
}
