#include "weather_snapshot.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static weather_snapshot_t s_value; static SemaphoreHandle_t s_lock;
esp_err_t weather_snapshot_init(void) {
    if (s_lock) return ESP_OK;
    memset(&s_value, 0, sizeof(s_value));
    s_lock=xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}
void weather_snapshot_get(weather_snapshot_t *out) { if(!out||!s_lock)return; xSemaphoreTake(s_lock,portMAX_DELAY);*out=s_value;xSemaphoreGive(s_lock); }
void weather_snapshot_set(const weather_snapshot_t *value) { if(!value||!s_lock)return; xSemaphoreTake(s_lock,portMAX_DELAY);s_value=*value;xSemaphoreGive(s_lock); }
