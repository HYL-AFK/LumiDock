#pragma once
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
/* 设置默认时区，不启动任何网络任务。 */
esp_err_t app_time_prepare_timezone(void);
/* 创建 SNTP/时区校时任务。 */
esp_err_t app_time_init(void);
/* 投递联网事件，由校时任务异步处理。 */
void app_time_notify_network(void);
/* Public reachability is confirmed. Release the location/weather side only once
 * per online transition; SNTP itself starts from the earlier STA/IP event. */
void app_time_notify_internet_online(void);
void app_time_apply_http_time(time_t utc_time);
bool app_time_is_initial_location_settled(void);
