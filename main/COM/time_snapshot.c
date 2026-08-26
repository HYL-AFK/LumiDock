#include "time_snapshot.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static time_snapshot_t s_value; static SemaphoreHandle_t s_lock; static bool s_time_ready;
esp_err_t time_snapshot_init(void) { if (s_lock) return ESP_OK; s_lock = xSemaphoreCreateMutex(); return s_lock ? ESP_OK : ESP_ERR_NO_MEM; }
bool time_snapshot_is_ready(void) { if (!s_lock) return false; xSemaphoreTake(s_lock, portMAX_DELAY); bool ready=s_time_ready; xSemaphoreGive(s_lock); return ready; }
void time_snapshot_get(time_snapshot_t *out) { if (!out || !s_lock) return; xSemaphoreTake(s_lock, portMAX_DELAY); *out=s_value; xSemaphoreGive(s_lock); }
void time_snapshot_set(const time_snapshot_t *value) { if (!value || !s_lock) return; xSemaphoreTake(s_lock, portMAX_DELAY); uint32_t revision=s_value.revision+1U; s_value=*value; s_value.revision=revision; s_time_ready=true; xSemaphoreGive(s_lock); }
