#include "watchdog_service.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "runtime_event_log.h"
#include "status_led_service.h"

#define WATCHDOG_TIMEOUT_MS 8000
#define WATCHDOG_SUPERVISOR_PERIOD_MS 250
#define WATCHDOG_STALE_THRESHOLD_MS 3000
#define WATCHDOG_MAX_TASKS 12
#define WATCHDOG_TASK_NAME_LENGTH 16

typedef struct {
    TaskHandle_t handle;
    int64_t last_feed_us;
    bool active;
    bool blocking;
    bool stale_reported;
    char name[WATCHDOG_TASK_NAME_LENGTH];
} watchdog_slot_t;

static const char *TAG = "watchdog";
static watchdog_slot_t s_slots[WATCHDOG_MAX_TASKS];
static SemaphoreHandle_t s_lock;
static bool s_initialized;
static bool s_business_suspended;
static bool s_failure_latched;

static watchdog_slot_t *find_slot(TaskHandle_t handle)
{
    for (size_t i = 0; i < WATCHDOG_MAX_TASKS; ++i) {
        if (s_slots[i].active && s_slots[i].handle == handle) return &s_slots[i];
    }
    return NULL;
}

static void supervisor_task(void *arg)
{
    (void)arg;
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "supervisor watchdog registration failed: %s", esp_err_to_name(err));
    }

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        bool stale = false;
        if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_business_suspended && !s_failure_latched) {
            for (size_t i = 0; i < WATCHDOG_MAX_TASKS; ++i) {
                watchdog_slot_t *slot = &s_slots[i];
                if (!slot->active || slot->blocking ||
                    now_us - slot->last_feed_us <
                        (int64_t)WATCHDOG_STALE_THRESHOLD_MS * 1000) {
                    continue;
                }
                if (!slot->stale_reported) {
                    slot->stale_reported = true;
                    stale = true;
                    ESP_LOGE(TAG, "application task heartbeat stale: %s", slot->name);
                    (void)runtime_event_log_append(RUNTIME_EVENT_WATCHDOG_FAILURE,
                                                   (int32_t)i,
                                                   WATCHDOG_STALE_THRESHOLD_MS,
                                                   0);
                    s_failure_latched = true;
                    status_led_service_set_critical_fault(true);
                    break;
                }
            }
        }
        if (s_lock != NULL) xSemaphoreGive(s_lock);

        /* Once a failure is latched, deliberately stop feeding the hardware WDT. */
        if (!s_failure_latched) {
            (void)esp_task_wdt_reset();
        } else if (stale) {
            ESP_LOGE(TAG, "watchdog reset armed after heartbeat failure");
        }
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_SUPERVISOR_PERIOD_MS));
    }
}

esp_err_t watchdog_service_init(void)
{
    if (s_initialized) return ESP_OK;

    const esp_task_wdt_config_t config = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_err_t err = esp_task_wdt_init(&config);
    if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_reconfigure(&config);
    if (err != ESP_OK) return err;

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    memset(s_slots, 0, sizeof(s_slots));
    s_business_suspended = false;
    s_failure_latched = false;
    s_initialized = true;

    if (xTaskCreate(supervisor_task, "watchdog_supervisor", 3072, NULL, 6,
                    NULL) != pdPASS) {
        s_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "task watchdog supervisor enabled timeout=%dms stale=%dms",
             WATCHDOG_TIMEOUT_MS, WATCHDOG_STALE_THRESHOLD_MS);
    return ESP_OK;
}

esp_err_t watchdog_service_subscribe_current_task(void)
{
    if (!s_initialized || s_lock == NULL) return ESP_ERR_INVALID_STATE;
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    watchdog_slot_t *slot = find_slot(current);
    if (slot == NULL) {
        for (size_t i = 0; i < WATCHDOG_MAX_TASKS; ++i) {
            if (!s_slots[i].active) {
                slot = &s_slots[i];
                memset(slot, 0, sizeof(*slot));
                slot->active = true;
                slot->handle = current;
                slot->last_feed_us = esp_timer_get_time();
                strlcpy(slot->name, pcTaskGetName(current), sizeof(slot->name));
                break;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return slot != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void watchdog_service_feed(void)
{
    if (!s_initialized || s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    watchdog_slot_t *slot = find_slot(xTaskGetCurrentTaskHandle());
    if (slot != NULL) {
        slot->last_feed_us = esp_timer_get_time();
        slot->stale_reported = false;
    }
    xSemaphoreGive(s_lock);
}

void watchdog_service_set_current_task_blocking(bool blocking)
{
    if (!s_initialized || s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    watchdog_slot_t *slot = find_slot(xTaskGetCurrentTaskHandle());
    if (slot != NULL) {
        slot->blocking = blocking;
        slot->last_feed_us = esp_timer_get_time();
        slot->stale_reported = false;
    }
    xSemaphoreGive(s_lock);
}

void watchdog_service_set_business_suspended(bool suspended)
{
    if (!s_initialized || s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_business_suspended = suspended;
    if (!suspended) {
        const int64_t baseline_us = esp_timer_get_time();
        for (size_t i = 0; i < WATCHDOG_MAX_TASKS; ++i) {
            watchdog_slot_t *slot = &s_slots[i];
            if (!slot->active) continue;
            slot->last_feed_us = baseline_us;
            slot->stale_reported = false;
        }
    }
    xSemaphoreGive(s_lock);
}
