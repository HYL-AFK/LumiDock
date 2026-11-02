#include "fault_registry.h"

#include <stddef.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "app_log.h"
#include "runtime_event_log.h"

#define FAULT_STORE_MAGIC 0x50464C54u /* "PFLT" */
#define FAULT_STORE_VERSION 1U
#define FAULT_NVS_NAMESPACE "fault_reg"
#define FAULT_NVS_KEY "table"
/* NVS 写入会禁用 SPI cache，在调用现场同步落盘会瞬时冻结所有 PSRAM 任务。
 * 和 runtime_event_log 一样走周期批量提交。 */
#define FAULT_FLUSH_PERIOD_US (1000 * 1000) /* 1 s */
/* 2020-01-01 00:00:00 UTC。EN 之前的 time() 返回 1970，这种值存进表里
 * 会让所有故障看起来都发生在五十多年前，不如明确记成"时间不可信"。 */
#define FAULT_EPOCH_FLOOR 1577836800LL

typedef struct __attribute__((packed)) {
    uint16_t code;
    uint8_t level;
    uint8_t active;
    /* 首次出现时刻（Unix 秒）。时间没同步时是 0，表示这条记录的时间不可信。 */
    uint32_t first_seen;
    /* 本次挂载时的开机秒数。只在本运行期内有意义，加载时统一归零——
     * 等级1 的 30 分钟窗口靠它算，而重启本身就是修复手段，重启后重新计时
     * 才是对的。49 天是 uint32 秒的上限，跑到那个时长设备早该重启过了。 */
    uint32_t active_since_s;
    /* 累计触发次数，含历史已恢复的那些。 */
    uint32_t count;
} fault_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t capacity;
    uint16_t count;
    uint16_t reserved;
} fault_header_t;

typedef struct __attribute__((packed)) {
    fault_header_t header;
    fault_entry_t entries[FAULT_REGISTRY_CAPACITY];
} fault_store_t;

_Static_assert(sizeof(fault_entry_t) == 16, "fault entry layout changed");
_Static_assert(sizeof(fault_header_t) == 12, "fault header layout changed");
_Static_assert(sizeof(fault_store_t) == 396, "fault NVS blob size changed");

/* 码值、等级、名字集中在这里定义。等级也从这张表取而不是从码值推算，
 * 这样将来真要调整某个码的等级，只改这一行就够了。 */
typedef struct {
    uint16_t code;
    fault_level_t level;
    const char *name;
} fault_meta_t;

static const fault_meta_t s_meta[] = {
    {FAULT_P101_WATCHDOG_LATCHED, FAULT_LEVEL_1, "P101_WATCHDOG_LATCHED"},
    {FAULT_P102_SNAPSHOT_STALLED, FAULT_LEVEL_1, "P102_SNAPSHOT_STALLED"},
    {FAULT_P103_DISPLAY_LOST, FAULT_LEVEL_1, "P103_DISPLAY_LOST"},
    {FAULT_P104_AUDIO_CODEC_LOST, FAULT_LEVEL_1, "P104_AUDIO_CODEC_LOST"},
    {FAULT_P105_HEAP_CRITICAL, FAULT_LEVEL_1, "P105_HEAP_CRITICAL"},

    {FAULT_P201_LOW_BATTERY, FAULT_LEVEL_2, "P201_LOW_BATTERY"},
    {FAULT_P202_WIFI_LOST, FAULT_LEVEL_2, "P202_WIFI_LOST"},
    {FAULT_P203_INTERNET_DOWN, FAULT_LEVEL_2, "P203_INTERNET_DOWN"},
    {FAULT_P204_TIME_UNSYNCED, FAULT_LEVEL_2, "P204_TIME_UNSYNCED"},
    {FAULT_P205_BMS_OFFLINE, FAULT_LEVEL_2, "P205_BMS_OFFLINE"},
    {FAULT_P206_RTC_ABSENT, FAULT_LEVEL_2, "P206_RTC_ABSENT"},
    {FAULT_P207_THERMAL_HIGH, FAULT_LEVEL_2, "P207_THERMAL_HIGH"},
    {FAULT_P208_STC_OFFLINE, FAULT_LEVEL_2, "P208_STC_OFFLINE"},

    {FAULT_P301_WEATHER_STALE, FAULT_LEVEL_3, "P301_WEATHER_STALE"},
    {FAULT_P302_NEWS_FAILED, FAULT_LEVEL_3, "P302_NEWS_FAILED"},
    {FAULT_P303_RADIO_RECONNECTING, FAULT_LEVEL_3, "P303_RADIO_RECONNECTING"},
    {FAULT_P304_VOICE_DEGRADED, FAULT_LEVEL_3, "P304_VOICE_DEGRADED"},
    {FAULT_P305_TARIFF_STALE, FAULT_LEVEL_3, "P305_TARIFF_STALE"},
};

#define FAULT_META_COUNT (sizeof(s_meta) / sizeof(s_meta[0]))

static const char *TAG = "fault_reg";
static fault_store_t s_store;
static SemaphoreHandle_t s_lock;
static bool s_initialized;
static bool s_dirty;
static esp_timer_handle_t s_flush_timer;

static void flush_timer_cb(void *arg);

static const fault_meta_t *find_meta(uint16_t code)
{
    for (size_t i = 0; i < FAULT_META_COUNT; ++i) {
        if (s_meta[i].code == code) return &s_meta[i];
    }
    return NULL;
}

const char *fault_code_name(uint16_t code)
{
    const fault_meta_t *meta = find_meta(code);
    return meta != NULL ? meta->name : "P???";
}

fault_level_t fault_code_level(uint16_t code)
{
    const fault_meta_t *meta = find_meta(code);
    return meta != NULL ? meta->level : FAULT_LEVEL_NONE;
}

static uint32_t now_epoch(void)
{
    const time_t now = time(NULL);
    return (int64_t)now >= FAULT_EPOCH_FLOOR ? (uint32_t)now : 0U;
}

static uint32_t uptime_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

static uint16_t used_slots(void)
{
    uint16_t used = 0;
    for (size_t i = 0; i < FAULT_REGISTRY_CAPACITY; ++i) {
        if (s_store.entries[i].code != 0U) used++;
    }
    return used;
}

static fault_entry_t *find_entry(uint16_t code)
{
    for (size_t i = 0; i < FAULT_REGISTRY_CAPACITY; ++i) {
        if (s_store.entries[i].code == code) return &s_store.entries[i];
    }
    return NULL;
}

/* 优先用空槽；没有空槽就回收已恢复记录里最早出现的那条。仍在挂载的记录
 * 一律不回收——把一个还在响的故障挤掉，等于把现场证据丢了。表满且全部
 * 在挂载时返回 NULL，调用方报 ESP_ERR_NO_MEM。 */
static fault_entry_t *alloc_entry(void)
{
    for (size_t i = 0; i < FAULT_REGISTRY_CAPACITY; ++i) {
        if (s_store.entries[i].code == 0U) return &s_store.entries[i];
    }

    fault_entry_t *oldest = NULL;
    for (size_t i = 0; i < FAULT_REGISTRY_CAPACITY; ++i) {
        fault_entry_t *entry = &s_store.entries[i];
        if (entry->active) continue;
        if (oldest == NULL || entry->first_seen < oldest->first_seen) oldest = entry;
    }
    return oldest;
}

static void store_reset(void)
{
    memset(&s_store, 0, sizeof(s_store));
    s_store.header.magic = FAULT_STORE_MAGIC;
    s_store.header.version = FAULT_STORE_VERSION;
    s_store.header.capacity = FAULT_REGISTRY_CAPACITY;
}

/* 读回来的表要做三件事：校验头部、丢掉码表里已经不存在的码、用码表里
 * 的等级覆盖存储值。第二件是为了固件升级——删掉一个码不该让整张表作废，
 * 只把那行清掉就行。 */
static bool store_validate(void)
{
    const fault_header_t *header = &s_store.header;
    if (header->magic != FAULT_STORE_MAGIC || header->version != FAULT_STORE_VERSION ||
        header->capacity != FAULT_REGISTRY_CAPACITY ||
        header->count > FAULT_REGISTRY_CAPACITY) {
        return false;
    }

    bool cleaned = false;
    for (size_t i = 0; i < FAULT_REGISTRY_CAPACITY; ++i) {
        fault_entry_t *entry = &s_store.entries[i];
        if (entry->code == 0U) continue;
        const fault_meta_t *meta = find_meta(entry->code);
        if (meta == NULL) {
            memset(entry, 0, sizeof(*entry));
            cleaned = true;
            continue;
        }
        entry->level = (uint8_t)meta->level;
    }

    if (cleaned) s_store.header.count = used_slots();
    return true;
}

static esp_err_t store_commit(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FAULT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, FAULT_NVS_KEY, &s_store, sizeof(s_store));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void flush_timer_cb(void *arg)
{
    (void)arg;
    if (!s_initialized || !s_dirty) return;

    /* 快速路径不持锁判断脏标志，确认后再短暂持锁批量落盘。 */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (s_dirty) {
        const esp_err_t err = store_commit();
        if (err == ESP_OK) {
            s_dirty = false;
        } else {
            APP_LOG_ERR(TAG, err, "fault table flush failed");
        }
    }
    xSemaphoreGive(s_lock);
}

esp_err_t fault_registry_init(void)
{
    if (s_initialized) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    uint16_t cleared_level1 = 0;
    bool valid = false;
    nvs_handle_t handle;
    if (nvs_open(FAULT_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(s_store);
        if (nvs_get_blob(handle, FAULT_NVS_KEY, &s_store, &size) == ESP_OK &&
            size == sizeof(s_store)) {
            valid = store_validate();
        }
        nvs_close(handle);
    }

    if (!valid) {
        store_reset();
        APP_LOGW(TAG, "fault table missing or unreadable; starting empty");
    } else {
        /* 跨重启保留挂载状态，但挂载时长必须归零：active_since_s 是相对
         * 本次开机的秒数，拿上次的值得出的差值没有意义。等级1 的 30 分钟
         * 窗口因此每次都从开机重新计。 */
        for (size_t i = 0; i < FAULT_REGISTRY_CAPACITY; ++i) {
            fault_entry_t *entry = &s_store.entries[i];
            entry->active_since_s = 0;
            /* 等级1 在开机时一律摘掉挂载位。它们的处置手段就是重启，而重启
             * 刚刚发生过——不清的话，一个导致重启的故障会在设备重新跑起来
             * 之后仍被算作“还挂着”，30 分钟后又触发一次，永远停在同一个
             * 循环里。first_seen 和 count 保留，出事之后仍然查得到它来过
             * 几次、第一次是什么时候。
             *
             * 等级2/3 则相反：低电量、断网、天气过期这些重启不会改变它们的
             * 存在，保留下来才能让用户开机就看到真正的问题。 */
            if (entry->active && entry->level == (uint8_t)FAULT_LEVEL_1) {
                entry->active = 0;
                cleared_level1++;
            }
        }
    }
    s_store.header.count = used_slots();
    s_initialized = true;

    esp_timer_create_args_t targs = {
        .callback = flush_timer_cb,
        .name = "fault_flush",
    };
    if (esp_timer_create(&targs, &s_flush_timer) == ESP_OK) {
        esp_timer_start_periodic(s_flush_timer, FAULT_FLUSH_PERIOD_US);
    }

    APP_LOGI(TAG, "fault registry ready: %u active of %u stored, codes=%u, level1 cleared on boot=%u",
             (unsigned)fault_registry_active_count(),
             (unsigned)s_store.header.count,
             (unsigned)FAULT_META_COUNT,
             (unsigned)cleared_level1);
    return ESP_OK;
}

esp_err_t fault_registry_raise(fault_code_t code, int32_t detail)
{
    if (!s_initialized || s_lock == NULL) return ESP_ERR_INVALID_STATE;
    const fault_level_t level = fault_code_level((uint16_t)code);
    if (level == FAULT_LEVEL_NONE) {
        APP_LOGW(TAG, "raise refused: unknown code 0x%03x", (unsigned)code);
        return ESP_ERR_INVALID_ARG;
    }

    bool fresh = false;
    uint16_t active = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    fault_entry_t *entry = find_entry((uint16_t)code);
    /* 已经挂着时直接返回。count 记的是“挂载了几次”，不是“被观测到几次”——
     * 断网这种持续状态如果每秒都算一次，计数会刷成天文数字，反而看不出
     * 到底发生过几回。调用方因此不需要先查 is_active，反复 raise 是幂等的。 */
    if (entry != NULL && entry->active) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (entry == NULL) {
        entry = alloc_entry();
        if (entry == NULL) {
            xSemaphoreGive(s_lock);
            APP_LOGW(TAG, "fault table full, %s dropped", fault_code_name((uint16_t)code));
            return ESP_ERR_NO_MEM;
        }
        memset(entry, 0, sizeof(*entry));
        entry->code = (uint16_t)code;
        entry->first_seen = now_epoch();
        s_store.header.count = used_slots();
        fresh = true;
    } else if (!entry->active) {
        /* 同一个故障二次出现：不重置 first_seen（那是"首次出现"的语义），
         * 但挂载计时要从此刻重来。 */
        fresh = true;
    }
    entry->level = (uint8_t)level;
    if (fresh) {
        entry->active = 1;
        entry->active_since_s = uptime_seconds();
    }
    entry->count++;
    s_dirty = true;
    for (size_t i = 0; i < FAULT_REGISTRY_CAPACITY; ++i) {
        if (s_store.entries[i].active) active++;
    }
    xSemaphoreGive(s_lock);

    /* 只在状态翻转时打日志和记流水。持续挂载的故障每被观测一次都刷一行，
     * 串口会被低电量这类长期项淹掉。 */
    if (fresh) {
        APP_LOGW(TAG, "fault raised: %s level=%d detail=%ld active=%u",
                 fault_code_name((uint16_t)code), (int)level, (long)detail,
                 (unsigned)active);
        (void)runtime_event_log_append(RUNTIME_EVENT_FAULT_RAISED, (int32_t)(uint16_t)code,
                                       detail, 0);
    }
    return ESP_OK;
}

esp_err_t fault_registry_clear(fault_code_t code)
{
    if (!s_initialized || s_lock == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    fault_entry_t *entry = find_entry((uint16_t)code);
    /* 未挂载时静默通过：恢复路径本来就可能被多走一遍，网络断开和连通都会
     * 触发一次状态刷新。 */
    if (entry == NULL || !entry->active) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    entry->active = 0;
    entry->active_since_s = 0;
    s_dirty = true;
    xSemaphoreGive(s_lock);

    APP_LOGI(TAG, "fault cleared: %s", fault_code_name((uint16_t)code));
    (void)runtime_event_log_append(RUNTIME_EVENT_FAULT_CLEARED, (int32_t)(uint16_t)code, 0, 0);
    return ESP_OK;
}

bool fault_registry_is_active(fault_code_t code)
{
    if (!s_initialized || s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const fault_entry_t *entry = find_entry((uint16_t)code);
    const bool active = entry != NULL && entry->active;
    xSemaphoreGive(s_lock);
    return active;
}

fault_level_t fault_registry_highest_level(void)
{
    if (!s_initialized || s_lock == NULL) return FAULT_LEVEL_NONE;

    fault_level_t highest = FAULT_LEVEL_NONE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < FAULT_REGISTRY_CAPACITY; ++i) {
        const fault_entry_t *entry = &s_store.entries[i];
        if (!entry->active) continue;
        const fault_level_t level = (fault_level_t)entry->level;
        if (level == FAULT_LEVEL_NONE) continue;
        /* 数字越小越严重，所以取最小值而不是最大值。 */
        if (highest == FAULT_LEVEL_NONE || level < highest) highest = level;
    }
    xSemaphoreGive(s_lock);
    return highest;
}

uint16_t fault_registry_active_count(void)
{
    if (!s_initialized || s_lock == NULL) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t active = 0;
    for (size_t i = 0; i < FAULT_REGISTRY_CAPACITY; ++i) {
        if (s_store.entries[i].active) active++;
    }
    xSemaphoreGive(s_lock);
    return active;
}

bool fault_registry_level1_overdue(void)
{
    if (!s_initialized || s_lock == NULL) return false;

    const uint32_t uptime_s = uptime_seconds();
    bool overdue = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < FAULT_REGISTRY_CAPACITY; ++i) {
        const fault_entry_t *entry = &s_store.entries[i];
        if (!entry->active || entry->level != (uint8_t)FAULT_LEVEL_1) continue;
        const uint32_t held_s = uptime_s - entry->active_since_s;
        if (held_s < FAULT_LEVEL1_REBOOT_TIMEOUT_S) continue;
        APP_LOGE(TAG, "level1 fault %s held for %lu s (limit %u s); reboot required",
                 fault_code_name(entry->code), (unsigned long)held_s,
                 (unsigned)FAULT_LEVEL1_REBOOT_TIMEOUT_S);
        overdue = true;
        break;
    }
    xSemaphoreGive(s_lock);
    return overdue;
}

esp_err_t fault_registry_clear_all(void)
{
    if (!s_initialized || s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    store_reset();
    const esp_err_t err = store_commit();
    if (err == ESP_OK) s_dirty = false;
    xSemaphoreGive(s_lock);
    APP_LOGI(TAG, "fault table cleared");
    return err;
}

void fault_registry_dump(void)
{
    if (!s_initialized || s_lock == NULL) return;

    const uint32_t uptime_s = uptime_seconds();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* 这里不能调 fault_registry_active_count()，它自己会拿同一把锁。 */
    uint16_t active = 0;
    for (size_t i = 0; i < FAULT_REGISTRY_CAPACITY; ++i) {
        if (s_store.entries[i].active) active++;
    }
    APP_LOGI(TAG, "fault table: %u active / %u stored",
             (unsigned)active, (unsigned)s_store.header.count);
    for (size_t i = 0; i < FAULT_REGISTRY_CAPACITY; ++i) {
        const fault_entry_t *entry = &s_store.entries[i];
        if (entry->code == 0U) continue;
        APP_LOGI(TAG, "  %s level=%u active=%u count=%lu first_seen=%lu held=%lu s",
                 fault_code_name(entry->code),
                 (unsigned)entry->level,
                 (unsigned)entry->active,
                 (unsigned long)entry->count,
                 (unsigned long)entry->first_seen,
                 (unsigned long)(entry->active ? uptime_s - entry->active_since_s : 0U));
    }
    xSemaphoreGive(s_lock);
}
