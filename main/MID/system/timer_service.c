#include "timer_service.h"

#include <limits.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TIMER_NVS_NAMESPACE "lumidock_timer"
#define TIMER_NVS_KEY_ALARMS "alarms"
#define TIMER_NVS_KEY_COUNTDOWNS "countdowns"

/* 2020-09-13: any clock newer than this is treated as SNTP-converged. */
#define TIMER_TIME_SYNCED_EPOCH 1600000000LL

static const char *const TAG = "timer_svc";

typedef struct {
    bool valid;
    int64_t target_epoch;
} timer_alarm_slot_t;

typedef struct {
    bool valid;
    int32_t remaining_seconds;
} timer_countdown_slot_t;

static timer_alarm_slot_t s_alarms[TIMER_SERVICE_MAX_ALARMS];
static timer_countdown_slot_t s_countdowns[TIMER_SERVICE_MAX_COUNTDOWNS];
/* Reserved by a voice command but deliberately invisible to poll/snapshot
 * until the acknowledgement TTS has finished (or the voice chain exits). */
static timer_countdown_slot_t s_pending_countdowns[TIMER_SERVICE_MAX_COUNTDOWNS];

/* Stopwatch: in-memory only, resets on power loss. */
static bool s_stopwatch_running;
static uint32_t s_stopwatch_accumulated_seconds;
static int64_t s_stopwatch_segment_start_us;

/* Ringing state. */
static bool s_ringing;
static timer_service_slot_t s_ringing_slot;
static bool s_ring_pending;
static timer_service_slot_t s_pending_slot;
static int64_t s_ring_started_us;
static int64_t s_last_heartbeat_us;

static timer_service_event_cb_t s_event_cb;
/* Countdown progress flush counter (NVS wear limiting). */
static uint32_t s_persist_tick;
/* Wall-clock stamp of the previous poll; countdown ticks use the real
 * elapsed seconds so a delayed task loop cannot slow the countdown. */
static time_t s_last_poll_time;

/* NVS persistence must not run on a PSRAM task stack: flash writes disable
 * the SPI cache and PSRAM stacks become unreachable, which trips
 * esp_task_stack_is_sane_cache_disabled().  The voice session task that
 * calls the timer APIs has a PSRAM stack, so callers only snapshot their
 * state under s_persist_mutex and wake s_persist_task, which performs the
 * real NVS write on an internal-RAM stack. */
#define TIMER_PERSIST_BIT_ALARMS (1UL << 0)
#define TIMER_PERSIST_BIT_COUNTDOWNS (1UL << 1)

static TaskHandle_t s_persist_task;
static SemaphoreHandle_t s_persist_mutex;
static SemaphoreHandle_t s_timer_load_done;
static timer_alarm_slot_t s_persist_alarms_shadow[TIMER_SERVICE_MAX_ALARMS];
static timer_countdown_slot_t s_persist_countdowns_shadow[TIMER_SERVICE_MAX_COUNTDOWNS];

static void request_persist(uint32_t bits)
{
    if (s_persist_task == NULL || s_persist_mutex == NULL) {
        ESP_LOGW(TAG, "persist worker unavailable; timer state stays in memory only");
        return;
    }
    if (xSemaphoreTake(s_persist_mutex, portMAX_DELAY) != pdTRUE) return;
    if (bits & TIMER_PERSIST_BIT_ALARMS) {
        memcpy(s_persist_alarms_shadow, s_alarms, sizeof(s_persist_alarms_shadow));
    }
    if (bits & TIMER_PERSIST_BIT_COUNTDOWNS) {
        memcpy(s_persist_countdowns_shadow, s_countdowns, sizeof(s_persist_countdowns_shadow));
    }
    xSemaphoreGive(s_persist_mutex);
    (void)xTaskNotify(s_persist_task, bits, eSetBits);
}

static esp_err_t persist_alarms(void)
{
    request_persist(TIMER_PERSIST_BIT_ALARMS);
    return ESP_OK;
}

static esp_err_t persist_countdowns(void)
{
    request_persist(TIMER_PERSIST_BIT_COUNTDOWNS);
    return ESP_OK;
}

static void load_persisted_state(void)
{
    nvs_handle_t handle;
    if (nvs_open(TIMER_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t blob_len = sizeof(s_alarms);
    if (nvs_get_blob(handle, TIMER_NVS_KEY_ALARMS, s_alarms, &blob_len) != ESP_OK) {
        memset(s_alarms, 0, sizeof(s_alarms));
    }
    blob_len = sizeof(s_countdowns);
    if (nvs_get_blob(handle, TIMER_NVS_KEY_COUNTDOWNS, s_countdowns, &blob_len) != ESP_OK) {
        memset(s_countdowns, 0, sizeof(s_countdowns));
    }
    nvs_close(handle);
    for (size_t i = 0; i < TIMER_SERVICE_MAX_ALARMS; ++i) {
        if (s_alarms[i].valid && s_alarms[i].target_epoch > 0) {
            ESP_LOGI(TAG, "alarm restored: slot=%u target=%lld", (unsigned)i,
                     (long long)s_alarms[i].target_epoch);
        }
    }
    for (size_t i = 0; i < TIMER_SERVICE_MAX_COUNTDOWNS; ++i) {
        if (s_countdowns[i].valid && s_countdowns[i].remaining_seconds > 0) {
            ESP_LOGI(TAG, "countdown restored: slot=%u remaining=%ld",
                     (unsigned)i, (long)s_countdowns[i].remaining_seconds);
        }
    }
}

/* NVS reads disable the SPI cache.  Keep this task's stack in internal RAM;
 * the voice initializer itself intentionally lives on a PSRAM stack. */
static void timer_load_task(void *arg)
{
    (void)arg;
    load_persisted_state();
    if (s_timer_load_done != NULL) xSemaphoreGive(s_timer_load_done);
    vTaskDelete(NULL);
}

static void flush_alarms_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TIMER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "alarm persist open failed: %s", esp_err_to_name(err));
        return;
    }
    const size_t blob_len = sizeof(s_persist_alarms_shadow);
    err = nvs_set_blob(handle, TIMER_NVS_KEY_ALARMS, s_persist_alarms_shadow, blob_len);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "alarm persist failed: %s", esp_err_to_name(err));
    }
}

static void flush_countdowns_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TIMER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "countdown persist open failed: %s", esp_err_to_name(err));
        return;
    }
    const size_t blob_len = sizeof(s_persist_countdowns_shadow);
    err = nvs_set_blob(handle, TIMER_NVS_KEY_COUNTDOWNS, s_persist_countdowns_shadow, blob_len);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "countdown persist failed: %s", esp_err_to_name(err));
    }
}

static void persist_worker_task(void *arg)
{
    for (;;) {
        uint32_t bits = 0;
        (void)xTaskNotifyWait(0, ULONG_MAX, &bits, portMAX_DELAY);
        if (bits == 0 || s_persist_mutex == NULL) continue;
        if (xSemaphoreTake(s_persist_mutex, portMAX_DELAY) != pdTRUE) continue;
        if (bits & TIMER_PERSIST_BIT_ALARMS) flush_alarms_to_nvs();
        if (bits & TIMER_PERSIST_BIT_COUNTDOWNS) flush_countdowns_to_nvs();
        xSemaphoreGive(s_persist_mutex);
    }
}

static void notify_event(timer_service_event_t event, timer_service_slot_t slot)
{
    if (s_event_cb != NULL) s_event_cb(event, slot);
}

static bool slot_is_countdown(timer_service_slot_t slot)
{
    return slot == TIMER_SERVICE_SLOT_COUNTDOWN0 || slot == TIMER_SERVICE_SLOT_COUNTDOWN1;
}

esp_err_t timer_service_init(timer_service_event_cb_t event_cb)
{
    s_event_cb = event_cb;
    memset(s_alarms, 0, sizeof(s_alarms));
    memset(s_countdowns, 0, sizeof(s_countdowns));
    memset(s_pending_countdowns, 0, sizeof(s_pending_countdowns));
    s_stopwatch_running = false;
    s_stopwatch_accumulated_seconds = 0U;
    s_stopwatch_segment_start_us = 0;
    s_ringing = false;
    s_ring_pending = false;
    s_last_poll_time = time(NULL);
    /* Persist worker must run on an internal-RAM stack: it performs the NVS
     * writes the PSRAM-stack caller threads cannot safely do themselves. */
    s_persist_mutex = xSemaphoreCreateMutex();
    s_timer_load_done = xSemaphoreCreateBinary();
    if (s_timer_load_done == NULL ||
        xTaskCreatePinnedToCoreWithCaps(timer_load_task, "timer_load", 3072, NULL, 2,
                                        NULL, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "persisted timer load task create failed; using empty timer state");
        if (s_timer_load_done != NULL) {
            vSemaphoreDelete(s_timer_load_done);
            s_timer_load_done = NULL;
        }
    } else {
        (void)xSemaphoreTake(s_timer_load_done, portMAX_DELAY);
        vSemaphoreDelete(s_timer_load_done);
        s_timer_load_done = NULL;
    }
    if (xTaskCreatePinnedToCoreWithCaps(persist_worker_task, "timer_persist", 4096, NULL, 2,
                                        &s_persist_task, 1,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_persist_task = NULL;
        ESP_LOGE(TAG, "persist worker create failed; timer persistence disabled");
    }
    ESP_LOGI(TAG, "timer service ready: alarms=%u countdowns=%u", TIMER_SERVICE_MAX_ALARMS,
             TIMER_SERVICE_MAX_COUNTDOWNS);
    return ESP_OK;
}

bool timer_service_is_time_synced(void)
{
    return time(NULL) > TIMER_TIME_SYNCED_EPOCH;
}

timer_service_cmd_result_t timer_service_set_alarm(uint8_t hour, uint8_t minute)
{
    if (hour > 23U || minute > 59U) return TIMER_SERVICE_CMD_FAILED;
    if (!timer_service_is_time_synced()) {
        ESP_LOGW(TAG, "alarm rejected: SNTP clock not synced yet");
        return TIMER_SERVICE_CMD_FAILED;
    }
    const time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    local.tm_hour = (int)hour;
    local.tm_min = (int)minute;
    local.tm_sec = 0;
    time_t target = mktime(&local);
    /* One-shot alarm: if the moment already passed, ring tomorrow. */
    if (target <= now + 60) target += 24 * 3600;
    for (size_t i = 0; i < TIMER_SERVICE_MAX_ALARMS; ++i) {
        if (!s_alarms[i].valid) {
            s_alarms[i].valid = true;
            s_alarms[i].target_epoch = (int64_t)target;
            persist_alarms();
            ESP_LOGI(TAG, "alarm set: slot=%u target=%lld", (unsigned)i, (long long)target);
            return TIMER_SERVICE_CMD_EXECUTED;
        }
    }
    ESP_LOGW(TAG, "alarm rejected: all %u slots in use", TIMER_SERVICE_MAX_ALARMS);
    return TIMER_SERVICE_CMD_FAILED;
}

timer_service_cmd_result_t timer_service_cancel_alarm(uint8_t hour, uint8_t minute)
{
    if (hour > 23U || minute > 59U) return TIMER_SERVICE_CMD_FAILED;
    bool removed = false;
    for (size_t i = 0; i < TIMER_SERVICE_MAX_ALARMS; ++i) {
        if (!s_alarms[i].valid) continue;
        const time_t target = (time_t)s_alarms[i].target_epoch;
        struct tm local;
        if (localtime_r(&target, &local) == NULL) continue;
        if (local.tm_hour == (int)hour && local.tm_min == (int)minute) {
            s_alarms[i].valid = false;
            s_alarms[i].target_epoch = 0;
            removed = true;
            ESP_LOGI(TAG, "alarm canceled: slot=%u time=%02u:%02u",
                     (unsigned)i, (unsigned)hour, (unsigned)minute);
        }
    }
    if (removed) {
        persist_alarms();
        return TIMER_SERVICE_CMD_EXECUTED;
    }
    ESP_LOGW(TAG, "alarm cancel: no matching alarm for %02u:%02u",
             (unsigned)hour, (unsigned)minute);
    return TIMER_SERVICE_CMD_FAILED;
}

timer_service_cmd_result_t timer_service_set_countdown(uint32_t duration_seconds)
{
    if (duration_seconds == 0U || duration_seconds > TIMER_SERVICE_MAX_DISPLAY_SECONDS) {
        return TIMER_SERVICE_CMD_FAILED;
    }
    if (s_stopwatch_running) {
        ESP_LOGW(TAG, "countdown rejected while stopwatch is running");
        return TIMER_SERVICE_CMD_FAILED;
    }
    for (size_t i = 0; i < TIMER_SERVICE_MAX_COUNTDOWNS; ++i) {
        if (!s_countdowns[i].valid && !s_pending_countdowns[i].valid) {
            s_countdowns[i].valid = true;
            s_countdowns[i].remaining_seconds = (int32_t)duration_seconds;
            persist_countdowns();
            ESP_LOGI(TAG, "countdown set: slot=%u duration=%lu",
                     (unsigned)i, (unsigned long)duration_seconds);
            return TIMER_SERVICE_CMD_EXECUTED;
        }
    }
    ESP_LOGW(TAG, "countdown rejected: all %u slots in use", TIMER_SERVICE_MAX_COUNTDOWNS);
    return TIMER_SERVICE_CMD_FAILED;
}

timer_service_cmd_result_t timer_service_prepare_countdown(uint32_t duration_seconds)
{
    if (duration_seconds == 0U || duration_seconds > TIMER_SERVICE_MAX_DISPLAY_SECONDS) {
        return TIMER_SERVICE_CMD_FAILED;
    }
    if (s_stopwatch_running) {
        ESP_LOGW(TAG, "countdown rejected while stopwatch is running");
        return TIMER_SERVICE_CMD_FAILED;
    }
    for (size_t i = 0; i < TIMER_SERVICE_MAX_COUNTDOWNS; ++i) {
        if (!s_countdowns[i].valid && !s_pending_countdowns[i].valid) {
            s_pending_countdowns[i].valid = true;
            s_pending_countdowns[i].remaining_seconds = (int32_t)duration_seconds;
            ESP_LOGI(TAG, "countdown prepared: slot=%u duration=%lu; waiting for TTS completion",
                     (unsigned)i, (unsigned long)duration_seconds);
            return TIMER_SERVICE_CMD_EXECUTED;
        }
    }
    ESP_LOGW(TAG, "countdown rejected: all %u slots in use or pending",
             TIMER_SERVICE_MAX_COUNTDOWNS);
    return TIMER_SERVICE_CMD_FAILED;
}

timer_service_cmd_result_t timer_service_start_pending_countdowns(void)
{
    bool started = false;
    for (size_t i = 0; i < TIMER_SERVICE_MAX_COUNTDOWNS; ++i) {
        if (!s_pending_countdowns[i].valid) continue;
        if (s_countdowns[i].valid) {
            ESP_LOGW(TAG, "pending countdown start conflict: slot=%u", (unsigned)i);
            s_pending_countdowns[i].valid = false;
            s_pending_countdowns[i].remaining_seconds = 0;
            continue;
        }
        s_countdowns[i] = s_pending_countdowns[i];
        s_pending_countdowns[i].valid = false;
        s_pending_countdowns[i].remaining_seconds = 0;
        persist_countdowns();
        ESP_LOGI(TAG, "countdown started after voice TTS/chain completion: slot=%u remaining=%ld",
                 (unsigned)i, (long)s_countdowns[i].remaining_seconds);
        started = true;
    }
    return started ? TIMER_SERVICE_CMD_EXECUTED : TIMER_SERVICE_CMD_FAILED;
}

void timer_service_cancel_pending_countdowns(void)
{
    bool canceled = false;
    for (size_t i = 0; i < TIMER_SERVICE_MAX_COUNTDOWNS; ++i) {
        if (!s_pending_countdowns[i].valid) continue;
        s_pending_countdowns[i].valid = false;
        s_pending_countdowns[i].remaining_seconds = 0;
        canceled = true;
    }
    if (canceled) ESP_LOGI(TAG, "pending voice countdowns canceled");
}

timer_service_cmd_result_t timer_service_start_stopwatch(void)
{
    if (s_stopwatch_running) return TIMER_SERVICE_CMD_FAILED;
    for (size_t i = 0; i < TIMER_SERVICE_MAX_COUNTDOWNS; ++i) {
        if (s_countdowns[i].valid && s_countdowns[i].remaining_seconds > 0) {
            ESP_LOGW(TAG, "stopwatch rejected while countdown is active");
            return TIMER_SERVICE_CMD_FAILED;
        }
    }
    s_stopwatch_running = true;
    s_stopwatch_segment_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "stopwatch started: accumulated=%lu",
             (unsigned long)s_stopwatch_accumulated_seconds);
    return TIMER_SERVICE_CMD_EXECUTED;
}

timer_service_cmd_result_t timer_service_pause_stopwatch(void)
{
    if (!s_stopwatch_running) return TIMER_SERVICE_CMD_FAILED;
    const int64_t segment_us = esp_timer_get_time() - s_stopwatch_segment_start_us;
    s_stopwatch_accumulated_seconds += (uint32_t)(segment_us / 1000000LL);
    s_stopwatch_running = false;
    s_stopwatch_segment_start_us = 0;
    ESP_LOGI(TAG, "stopwatch paused: accumulated=%lu",
             (unsigned long)s_stopwatch_accumulated_seconds);
    return TIMER_SERVICE_CMD_EXECUTED;
}

timer_service_cmd_result_t timer_service_stop_stopwatch(void)
{
    s_stopwatch_running = false;
    s_stopwatch_segment_start_us = 0;
    s_stopwatch_accumulated_seconds = 0U;
    ESP_LOGI(TAG, "stopwatch stopped and cleared");
    return TIMER_SERVICE_CMD_EXECUTED;
}

bool timer_service_query_stopwatch(uint32_t *seconds_out)
{
    if (seconds_out == NULL) return false;
    uint32_t total = s_stopwatch_accumulated_seconds;
    if (s_stopwatch_running && s_stopwatch_segment_start_us != 0) {
        const int64_t segment_us = esp_timer_get_time() - s_stopwatch_segment_start_us;
        total += (uint32_t)(segment_us / 1000000LL);
    }
    *seconds_out = total;
    return true;
}

bool timer_service_query_countdown_remaining(uint32_t *seconds_out)
{
    if (seconds_out == NULL) return false;
    uint32_t smallest = 0U;
    for (size_t i = 0; i < TIMER_SERVICE_MAX_COUNTDOWNS; ++i) {
        if (s_countdowns[i].valid && s_countdowns[i].remaining_seconds > 0) {
            if (smallest == 0U || (uint32_t)s_countdowns[i].remaining_seconds < smallest) {
                smallest = (uint32_t)s_countdowns[i].remaining_seconds;
            }
        }
    }
    if (smallest == 0U) return false;
    *seconds_out = smallest;
    return true;
}

void timer_service_get_snapshot(timer_service_snapshot_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    int64_t earliest_alarm = 0;
    for (size_t i = 0; i < TIMER_SERVICE_MAX_ALARMS; ++i) {
        if (!s_alarms[i].valid || s_alarms[i].target_epoch <= 0) continue;
        if (earliest_alarm == 0 || s_alarms[i].target_epoch < earliest_alarm) {
            earliest_alarm = s_alarms[i].target_epoch;
        }
    }
    if (earliest_alarm != 0) {
        const time_t target = (time_t)earliest_alarm;
        struct tm local;
        localtime_r(&target, &local);
        out->alarm_active = true;
        out->alarm_hour = (uint8_t)local.tm_hour;
        out->alarm_minute = (uint8_t)local.tm_min;
    }
    out->countdown_active = timer_service_query_countdown_remaining(&out->countdown_remaining_seconds);
    (void)timer_service_query_stopwatch(&out->stopwatch_seconds);
    out->stopwatch_running = s_stopwatch_running &&
                             out->stopwatch_seconds < TIMER_SERVICE_MAX_DISPLAY_SECONDS;
    if (out->stopwatch_seconds > TIMER_SERVICE_MAX_DISPLAY_SECONDS) {
        out->stopwatch_seconds = TIMER_SERVICE_MAX_DISPLAY_SECONDS;
    }
    out->ringing = s_ringing;
}

bool timer_service_is_ringing(void)
{
    return s_ringing;
}

void timer_service_stop_ringing(void)
{
    if (!s_ringing) return;
    s_ringing = false;
    s_ring_pending = false;
    ESP_LOGI(TAG, "ringing stopped: slot=%u", (unsigned)s_ringing_slot);
    notify_event(TIMER_SERVICE_EVENT_STOPPED, s_ringing_slot);
}

static void start_ringing(timer_service_slot_t slot)
{
    /* One chime at a time: queue a second expiry while the first rings. */
    if (s_ringing) {
        if (!s_ring_pending) {
            s_ring_pending = true;
            s_pending_slot = slot;
            ESP_LOGI(TAG, "ring queued: slot=%u (current=%u)", (unsigned)slot,
                     (unsigned)s_ringing_slot);
        }
        return;
    }
    s_ringing = true;
    s_ringing_slot = slot;
    s_ring_started_us = esp_timer_get_time();
    s_last_heartbeat_us = 0;
    /* Delete one-shot sources immediately: the alarm/countdown is consumed. */
    if (slot_is_countdown(slot)) {
        s_countdowns[slot - TIMER_SERVICE_SLOT_COUNTDOWN0].valid = false;
        s_countdowns[slot - TIMER_SERVICE_SLOT_COUNTDOWN0].remaining_seconds = 0;
        persist_countdowns();
    } else {
        s_alarms[slot - TIMER_SERVICE_SLOT_ALARM0].valid = false;
        s_alarms[slot - TIMER_SERVICE_SLOT_ALARM0].target_epoch = 0;
        persist_alarms();
    }
    ESP_LOGI(TAG, "timer ringing: slot=%u", (unsigned)slot);
    notify_event(TIMER_SERVICE_EVENT_RING, slot);
}

void timer_service_poll(void)
{
    const int64_t now_us = esp_timer_get_time();
    const time_t now = time(NULL);

    /* Real elapsed seconds since the previous poll (clamped so a clock jump
     * or task stall cannot silently consume a long countdown). */
    time_t elapsed = 1;
    if (s_last_poll_time > 0 && now >= s_last_poll_time) {
        elapsed = now - s_last_poll_time;
        if (elapsed < 1) elapsed = 1;
        if (elapsed > 60) elapsed = 60;
    }
    s_last_poll_time = now;

    if (s_stopwatch_running && s_stopwatch_segment_start_us != 0) {
        const uint32_t elapsed = (uint32_t)((now_us - s_stopwatch_segment_start_us) / 1000000LL);
        if (s_stopwatch_accumulated_seconds + elapsed >= TIMER_SERVICE_MAX_DISPLAY_SECONDS) {
            s_stopwatch_accumulated_seconds = TIMER_SERVICE_MAX_DISPLAY_SECONDS;
            s_stopwatch_segment_start_us = 0;
            s_stopwatch_running = false;
            ESP_LOGI(TAG, "stopwatch reached display limit: 59:59");
        }
    }

    /* Alarm expiry. */
    if (now > TIMER_TIME_SYNCED_EPOCH) {
        for (size_t i = 0; i < TIMER_SERVICE_MAX_ALARMS; ++i) {
            if (s_alarms[i].valid && (time_t)s_alarms[i].target_epoch <= now) {
                start_ringing((timer_service_slot_t)(TIMER_SERVICE_SLOT_ALARM0 + i));
            }
        }
    }

    /* Countdown tick and expiry. */
    bool countdown_changed = false;
    for (size_t i = 0; i < TIMER_SERVICE_MAX_COUNTDOWNS; ++i) {
        if (!s_countdowns[i].valid || s_countdowns[i].remaining_seconds <= 0) continue;
        if ((time_t)s_countdowns[i].remaining_seconds <= elapsed) {
            start_ringing((timer_service_slot_t)(TIMER_SERVICE_SLOT_COUNTDOWN0 + i));
            countdown_changed = true;
        } else {
            s_countdowns[i].remaining_seconds -= (int32_t)elapsed;
            countdown_changed = true;
        }
    }
    if (countdown_changed) {
        ++s_persist_tick;
        if (s_persist_tick >= TIMER_SERVICE_COUNTDOWN_PERSIST_TICKS) {
            s_persist_tick = 0U;
            persist_countdowns();
        }
    }

    /* Ringing heartbeat, auto-stop, and pending-ring handoff. */
    if (s_ringing) {
        if (now_us - s_ring_started_us >=
            (int64_t)TIMER_SERVICE_RING_TIMEOUT_MS * 1000LL) {
            s_ringing = false;
            ESP_LOGI(TAG, "ring auto-stopped: slot=%u", (unsigned)s_ringing_slot);
            notify_event(TIMER_SERVICE_EVENT_STOPPED, s_ringing_slot);
        } else if (s_last_heartbeat_us == 0 ||
                   now_us - s_last_heartbeat_us >=
                       (int64_t)TIMER_SERVICE_RING_HEARTBEAT_MS * 1000LL) {
            s_last_heartbeat_us = now_us;
            notify_event(TIMER_SERVICE_EVENT_RING, s_ringing_slot);
        }
    }
    if (!s_ringing && s_ring_pending) {
        s_ring_pending = false;
        start_ringing(s_pending_slot);
    }
}
