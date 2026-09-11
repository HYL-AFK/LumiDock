#include "runtime_event_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "nvs.h"
#include "app_log.h"

#define RUNTIME_EVENT_LOG_CAPACITY 16
#define RUNTIME_EVENT_RECORD_SIZE 32
#define RUNTIME_EVENT_MAGIC 0x524Cu
#define RUNTIME_EVENT_STORE_MAGIC 0x52544C47u
#define RUNTIME_EVENT_VERSION 1
#define RUNTIME_EVENT_NVS_NAMESPACE "runtime_log"
#define RUNTIME_EVENT_NVS_KEY "events"
#define RUNTIME_EVENT_TIME_VALID_FLAG (1u << 0)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint16_t capacity;
    uint16_t write_index;
    uint16_t read_index;
    uint16_t count;
    uint8_t reserved[8];
    uint32_t next_sequence;
    uint32_t header_crc32;
} runtime_event_header_t;

typedef struct __attribute__((packed)) {
    runtime_event_header_t header;
    runtime_event_record_t records[RUNTIME_EVENT_LOG_CAPACITY];
} runtime_event_store_t;

_Static_assert(sizeof(runtime_event_header_t) == 32,
               "runtime event header layout changed");
_Static_assert(sizeof(runtime_event_record_t) == RUNTIME_EVENT_RECORD_SIZE,
               "runtime event record layout changed");
_Static_assert(sizeof(runtime_event_store_t) == 544,
               "runtime event NVS blob size changed");

/* NVS 写入会禁用 SPI cache。事件先写入内存并标记脏，由周期定时任务批量落盘，
 * 避免在调用现场（如 BLE 窗口过期）瞬时冻结所有 PSRAM 任务造成卡顿。 */
#define RUNTIME_EVENT_FLUSH_PERIOD_US (1000 * 1000) /* 1 s */

static const char *TAG = "runtime_event_log";
static runtime_event_store_t s_store;
static SemaphoreHandle_t s_lock;
static bool s_initialized;
static bool s_dirty;
static esp_timer_handle_t s_flush_timer;

static void runtime_event_log_flush_timer_cb(void *arg);

static const char *event_type_name(runtime_event_type_t type)
{
    switch (type) {
    case RUNTIME_EVENT_BOOT: return "BOOT";
    case RUNTIME_EVENT_WIFI_CONNECTED: return "WIFI_CONNECTED";
    case RUNTIME_EVENT_WIFI_FAILURE: return "WIFI_FAILURE";
    case RUNTIME_EVENT_SNTP_SYNC: return "SNTP_SYNC";
    case RUNTIME_EVENT_WEATHER_SUCCESS: return "WEATHER_OK";
    case RUNTIME_EVENT_WEATHER_FAILURE: return "WEATHER_FAIL";
    case RUNTIME_EVENT_BRIGHTNESS_CHANGE: return "BRIGHTNESS";
    case RUNTIME_EVENT_DISPLAY_ERROR: return "DISPLAY_ERROR";
    case RUNTIME_EVENT_INTERNET_PROBE: return "INTERNET_PROBE";
    case RUNTIME_EVENT_LOCATION_SUCCESS: return "LOCATION_OK";
    case RUNTIME_EVENT_LOCATION_FAILURE: return "LOCATION_FAIL";
    case RUNTIME_EVENT_BLE_WINDOW_OPEN: return "BLE_WINDOW";
    case RUNTIME_EVENT_BLE_TIMEOUT: return "BLE_TIMEOUT";
    case RUNTIME_EVENT_BLE_PROVISION_ACCEPTED: return "BLE_ACCEPTED";
    case RUNTIME_EVENT_WATCHDOG_FAILURE: return "WATCHDOG";
    default: return "UNKNOWN";
    }
}

static uint32_t crc32(const void *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^
                  (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static uint32_t header_crc(const runtime_event_header_t *header)
{
    return crc32(header, offsetof(runtime_event_header_t, header_crc32));
}

static uint32_t record_crc(const runtime_event_record_t *record)
{
    return crc32(record, offsetof(runtime_event_record_t, record_crc32));
}

static bool record_valid(const runtime_event_record_t *record)
{
    return record->magic == RUNTIME_EVENT_MAGIC &&
           record->version == RUNTIME_EVENT_VERSION &&
           record->record_crc32 == record_crc(record);
}

static bool store_valid(const runtime_event_store_t *store)
{
    const runtime_event_header_t *header = &store->header;
    if (header->magic != RUNTIME_EVENT_STORE_MAGIC ||
        header->version != RUNTIME_EVENT_VERSION ||
        header->record_size != RUNTIME_EVENT_RECORD_SIZE ||
        header->capacity != RUNTIME_EVENT_LOG_CAPACITY ||
        header->write_index >= RUNTIME_EVENT_LOG_CAPACITY ||
        header->read_index >= RUNTIME_EVENT_LOG_CAPACITY ||
        header->count > RUNTIME_EVENT_LOG_CAPACITY ||
        header->header_crc32 != header_crc(header)) {
        return false;
    }

    for (uint16_t i = 0; i < header->count; ++i) {
        const uint16_t index =
            (uint16_t)((header->read_index + i) % RUNTIME_EVENT_LOG_CAPACITY);
        if (!record_valid(&store->records[index])) return false;
    }
    return true;
}

static void store_reset(runtime_event_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->header.magic = RUNTIME_EVENT_STORE_MAGIC;
    store->header.version = RUNTIME_EVENT_VERSION;
    store->header.record_size = RUNTIME_EVENT_RECORD_SIZE;
    store->header.capacity = RUNTIME_EVENT_LOG_CAPACITY;
    store->header.next_sequence = 1;
    store->header.header_crc32 = header_crc(&store->header);
}

static esp_err_t store_commit(const runtime_event_store_t *store)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(RUNTIME_EVENT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, RUNTIME_EVENT_NVS_KEY, store, sizeof(*store));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t runtime_event_log_init(void)
{
    if (s_initialized) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    bool valid = false;
    nvs_handle_t handle;
    if (nvs_open(RUNTIME_EVENT_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(s_store);
        if (nvs_get_blob(handle, RUNTIME_EVENT_NVS_KEY, &s_store, &size) == ESP_OK &&
            size == sizeof(s_store)) {
            valid = store_valid(&s_store);
        }
        nvs_close(handle);
    }

    if (!valid) {
        store_reset(&s_store);
        APP_LOGW(TAG, "event queue invalid or empty; initialized new queue");
    }
    s_initialized = true;
    APP_LOGI(TAG, "event queue ready: capacity=%u record_size=%u bytes=%u",
             RUNTIME_EVENT_LOG_CAPACITY,
             RUNTIME_EVENT_RECORD_SIZE,
             (unsigned)sizeof(s_store));

    /* 周期批量落盘，避免每次 append 同步写 NVS 禁用 cache 造成瞬时卡顿。 */
    esp_timer_create_args_t targs = {
        .callback = runtime_event_log_flush_timer_cb,
        .name = "rtlog_flush",
    };
    if (esp_timer_create(&targs, &s_flush_timer) == ESP_OK) {
        esp_timer_start_periodic(s_flush_timer, RUNTIME_EVENT_FLUSH_PERIOD_US);
    }
    return ESP_OK;
}

esp_err_t runtime_event_log_append(runtime_event_type_t type,
                                   int32_t value1,
                                   int32_t value2,
                                   uint32_t flags)
{
    if (!s_initialized || type == 0) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint16_t index = s_store.header.write_index;
    runtime_event_record_t *record = &s_store.records[index];
    memset(record, 0, sizeof(*record));
    record->magic = RUNTIME_EVENT_MAGIC;
    record->version = RUNTIME_EVENT_VERSION;
    record->event_type = (uint8_t)type;
    const time_t now = time(NULL);
    if (now >= 1577836800) {
        record->timestamp = (uint64_t)now;
        flags |= RUNTIME_EVENT_TIME_VALID_FLAG;
    }
    record->value1 = value1;
    record->value2 = value2;
    record->flags = flags;
    record->sequence = s_store.header.next_sequence++;
    record->record_crc32 = record_crc(record);

    s_store.header.write_index =
        (uint16_t)((index + 1U) % RUNTIME_EVENT_LOG_CAPACITY);
    if (s_store.header.count < RUNTIME_EVENT_LOG_CAPACITY) {
        s_store.header.count++;
    } else {
        s_store.header.read_index =
            (uint16_t)((s_store.header.read_index + 1U) % RUNTIME_EVENT_LOG_CAPACITY);
    }
    s_store.header.header_crc32 = header_crc(&s_store.header);

    /* 仅在内存更新并标记脏，真正落盘交给周期定时任务批量提交。 */
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void runtime_event_log_flush_timer_cb(void *arg)
{
    (void)arg;
    if (!s_initialized || !s_dirty) return;

    /* 快速路径不持锁判断脏标志，确认后再短暂持锁批量落盘。 */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (s_dirty) {
        esp_err_t err = store_commit(&s_store);
        if (err == ESP_OK) {
            s_dirty = false;
        } else {
            APP_LOG_ERR(TAG, err, "event flush failed");
        }
    }
    xSemaphoreGive(s_lock);
}

esp_err_t runtime_event_log_get(uint16_t offset_from_oldest,
                                runtime_event_record_t *out)
{
    if (!s_initialized || out == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (offset_from_oldest >= s_store.header.count) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    const uint16_t index = (uint16_t)(
        (s_store.header.read_index + offset_from_oldest) % RUNTIME_EVENT_LOG_CAPACITY);
    *out = s_store.records[index];
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

uint16_t runtime_event_log_count(void)
{
    if (!s_initialized) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint16_t count = s_store.header.count;
    xSemaphoreGive(s_lock);
    return count;
}

esp_err_t runtime_event_log_clear(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    runtime_event_store_t previous = s_store;
    store_reset(&s_store);
    esp_err_t err = store_commit(&s_store);
    if (err != ESP_OK) s_store = previous;
    xSemaphoreGive(s_lock);
    return err;
}

void runtime_event_log_dump_recent(uint16_t max_records)
{
    if (!s_initialized || max_records == 0U) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint16_t available = s_store.header.count;
    const uint16_t count = available < max_records ? available : max_records;
    const uint16_t offset = (uint16_t)(available - count);
    APP_LOGI(TAG, "persistent event history: showing %u of %u", count, available);
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t index = (uint16_t)((s_store.header.read_index + offset + i) %
                                          RUNTIME_EVENT_LOG_CAPACITY);
        const runtime_event_record_t *record = &s_store.records[index];
        APP_LOGI(TAG,
                 "event seq=%lu type=%s ts=%llu v1=%ld v2=%ld flags=0x%08lx",
                 (unsigned long)record->sequence,
                 event_type_name((runtime_event_type_t)record->event_type),
                 (unsigned long long)record->timestamp,
                 (long)record->value1,
                 (long)record->value2,
                 (unsigned long)record->flags);
    }
    xSemaphoreGive(s_lock);
}
