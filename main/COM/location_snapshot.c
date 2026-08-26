#include "location_snapshot.h"

#include <string.h>

#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static location_snapshot_t s_value;
static SemaphoreHandle_t s_lock;
#define LOCATION_NVS_NAMESPACE "location"
#define LOCATION_NVS_BLOB "coords"
#define LOCATION_NVS_TZ "timezone"

typedef struct {
    double latitude;
    double longitude;
} location_coords_t;

esp_err_t location_snapshot_init(void)
{
    if (s_lock != NULL) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    nvs_handle_t nvs;
    if (nvs_open(LOCATION_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        location_coords_t coords = {0};
        size_t size = sizeof(coords);
        char timezone[LOCATION_TIMEZONE_MAX_LEN] = {0};
        size_t tz_size = sizeof(timezone);
        if (nvs_get_blob(nvs, LOCATION_NVS_BLOB, &coords, &size) == ESP_OK &&
            size == sizeof(coords) && nvs_get_str(nvs, LOCATION_NVS_TZ, timezone, &tz_size) == ESP_OK) {
            s_value.latitude = coords.latitude;
            s_value.longitude = coords.longitude;
            strlcpy(s_value.timezone, timezone, sizeof(s_value.timezone));
            s_value.valid = true;
        }
        nvs_close(nvs);
    }
    return ESP_OK;
}

bool location_snapshot_get(location_snapshot_t *out)
{
    if (out == NULL || s_lock == NULL) return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_value;
    xSemaphoreGive(s_lock);
    return out->valid;
}

esp_err_t location_snapshot_set(double latitude, double longitude,
                                const char *timezone)
{
    if (s_lock == NULL || timezone == NULL || timezone[0] == '\0' ||
        latitude < -90.0 || latitude > 90.0 ||
        longitude < -180.0 || longitude > 180.0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_value.latitude = latitude;
    s_value.longitude = longitude;
    strlcpy(s_value.timezone, timezone, sizeof(s_value.timezone));
    s_value.valid = true;
    s_value.revision++;
    xSemaphoreGive(s_lock);

    nvs_handle_t nvs;
    if (nvs_open(LOCATION_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        const location_coords_t coords = { latitude, longitude };
        (void)nvs_set_blob(nvs, LOCATION_NVS_BLOB, &coords, sizeof(coords));
        (void)nvs_set_str(nvs, LOCATION_NVS_TZ, timezone);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
    return ESP_OK;
}
