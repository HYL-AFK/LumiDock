#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* 创建天气请求任务和天气快照缓存。 */
esp_err_t app_weather_init(void);
/* 通知天气任务立即刷新。 */
void app_weather_notify_network(void);
TaskHandle_t app_weather_get_task_handle(void);
bool app_weather_is_initial_request_settled(void);
