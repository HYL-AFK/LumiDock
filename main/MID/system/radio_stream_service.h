#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* 固定电台列表的第一版接口，避免模型直接下发任意网络地址。 */
esp_err_t radio_stream_service_init(void);
esp_err_t radio_stream_service_play_default(void);
/* 暂停是“静音保活”：HTTP 与解码器继续跑，只是不出声，所以“继续播放”能立刻恢复
   而不必重连。真正终止电台的只有 stop()。 */
esp_err_t radio_stream_service_pause(void);
esp_err_t radio_stream_service_resume(void);
esp_err_t radio_stream_service_stop(void);
/* 语音要占用扬声器（唤醒应答/提示音/错误音）时先把电台让位：电台会停掉拉流与
   解码、关掉 HTTP 连接，把 socket、WiFi 接收缓冲和 core 1 的解码时间整段让给
   语音链路，等 regain() 再重新接回直播。让位不等于终止：让位期间
   is_requested() 仍为真，答完会自动恢复。 */
esp_err_t radio_stream_service_yield(void);
esp_err_t radio_stream_service_regain(void);
/* 电台流当前是否真的在拉在放：让位期间和用户暂停期间都为假。 */
bool radio_stream_service_is_active(void);
/* 用户/云端是否要求电台在播：从 play_default() 到 stop()/pause() 之间为真，
   包含还在 CONNECTING、尚未出声、以及被语音让位的阶段。语音侧靠它判断
   “当前是电台模式”，设备状态上报也用它。 */
bool radio_stream_service_is_requested(void);

/* 读取并消费一次性电台失败状态，供下一次唤醒记录。 */
bool radio_stream_service_consume_failure(char *stage, size_t stage_size,
                                          int *error_code, uint8_t *reconnects);
