#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* 固定电台列表的第一版接口，避免模型直接下发任意网络地址。 */
esp_err_t radio_stream_service_init(void);
esp_err_t radio_stream_service_play_default(void);
esp_err_t radio_stream_service_pause(void);
esp_err_t radio_stream_service_resume(void);
esp_err_t radio_stream_service_stop(void);
bool radio_stream_service_is_active(void);

/* 读取并消费一次性电台失败状态，供下一次唤醒记录。 */
bool radio_stream_service_consume_failure(char *stage, size_t stage_size,
                                          int *error_code, uint8_t *reconnects);
