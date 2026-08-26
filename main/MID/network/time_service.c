#include "time_service.h"

#include <string.h>
#include <time.h>

#include "app_time.h"
#include "time_snapshot.h"
#include "mid_snapshot.h"

esp_err_t time_service_init(void)
{
    esp_err_t err = time_snapshot_init();
    if (err != ESP_OK) return err;
    return app_time_init();
}

void time_service_poll(void)
{
    if (!time_snapshot_is_ready()) return;
    lumidock_snapshot_t current;
    mid_snapshot_get(&current);
    time_weather_snapshot_t value = current.time_weather;
    const time_t now = time(NULL);
    value.valid = true;
    localtime_r(&now, &value.local_time);
    (void)strlcpy(value.timezone, "CST-8", sizeof(value.timezone));
    mid_snapshot_set_time_weather(&value);
}

void time_service_notify_network(void)
{
    app_time_notify_network();
}

void time_service_notify_internet_online(void)
{
    app_time_notify_internet_online();
}

void time_service_apply_http_time(time_t utc_time)
{
    app_time_apply_http_time(utc_time);
}

bool time_service_is_initial_location_settled(void)
{
    return app_time_is_initial_location_settled();
}
