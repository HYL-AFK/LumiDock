#include "weather_service.h"

#include <string.h>

#include "app_weather.h"
#include "weather_snapshot.h"
#include "mid_snapshot.h"

static const char *weather_text(int code)
{
    if (code == 0) return "晴";
    if (code <= 3) return "云";
    if (code == 45 || code == 48) return "雾";
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "雨";
    if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) return "雪";
    if (code == 95) return "雷";
    if (code >= 96) return "雷";
    return "未知";
}

esp_err_t weather_service_init(void)
{
    return app_weather_init();
}

void weather_service_poll(void)
{
    weather_snapshot_t source = {0};
    weather_snapshot_get(&source);
    lumidock_snapshot_t current;
    mid_snapshot_get(&current);
    time_weather_snapshot_t value = current.time_weather;
    if (source.valid && source.temperature_valid) {
        /* `valid` belongs to the clock/date portion. Weather can arrive
         * before or after SNTP and must not make an unsynchronised clock look
         * valid. */
        value.temperature_c = source.temperature_c;
        value.weather_code = source.weather_code;
        (void)strlcpy(value.weather_text, weather_text(source.weather_code),
                      sizeof(value.weather_text));
    }
    mid_snapshot_set_time_weather(&value);
}

void weather_service_notify_network(void)
{
    app_weather_notify_network();
}

bool weather_service_is_initial_request_settled(void)
{
    return app_weather_is_initial_request_settled();
}
