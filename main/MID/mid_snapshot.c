#include "mid_snapshot.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static lumidock_snapshot_t s_snapshot;
static SemaphoreHandle_t s_lock;

esp_err_t mid_snapshot_init(void)
{
    if (s_lock != NULL) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    (void)strlcpy(s_snapshot.time_weather.timezone, "CST-8", sizeof(s_snapshot.time_weather.timezone));
    return ESP_OK;
}

bool mid_snapshot_read_acquire(const lumidock_snapshot_t **out)
{
    if (out == NULL || s_lock == NULL) return false;
    /* The display owns its refresh cadence. A busy writer must not freeze the
     * startup animation or the current page, so callers retry next frame. */
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return false;
    *out = &s_snapshot;
    return true;
}

void mid_snapshot_read_release(void)
{
    if (s_lock != NULL) xSemaphoreGive(s_lock);
}

void mid_snapshot_get(lumidock_snapshot_t *out)
{
    if (out == NULL || s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snapshot;
    xSemaphoreGive(s_lock);
}

#define SNAPSHOT_SET(field) \
    do { if ((value) == NULL || s_lock == NULL) return; xSemaphoreTake(s_lock, portMAX_DELAY); s_snapshot.field = *(value); xSemaphoreGive(s_lock); } while (0)

void mid_snapshot_set_charging(const charging_snapshot_t *value) { SNAPSHOT_SET(charging); }
void mid_snapshot_set_bms(const bms_snapshot_t *value) { SNAPSHOT_SET(bms); }
void mid_snapshot_set_stc(const stc_snapshot_t *value) { SNAPSHOT_SET(stc); }
void mid_snapshot_set_wifi(const wifi_snapshot_t *value) { SNAPSHOT_SET(wifi); }
void mid_snapshot_set_bluetooth(const bluetooth_snapshot_t *value) { SNAPSHOT_SET(bluetooth); }
void mid_snapshot_set_voice_activation(const voice_activation_snapshot_t *value) { SNAPSHOT_SET(voice_activation); }
void mid_snapshot_set_voice_session(const voice_session_snapshot_t *value) { SNAPSHOT_SET(voice_session); }
void mid_snapshot_set_time_weather(const time_weather_snapshot_t *value) { SNAPSHOT_SET(time_weather); }
void mid_snapshot_set_startup_complete(bool complete)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot.startup_complete = complete;
    xSemaphoreGive(s_lock);
}
