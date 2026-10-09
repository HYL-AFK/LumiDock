#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* 固定电台列表的接口，避免模型直接下发任意网络地址。频道表在 .c 里（全是央广 HLS）。 */
esp_err_t radio_stream_service_init(void);
/* 默认频道（中国之声）。用户只说“打开电台”没点名字时走这个。 */
esp_err_t radio_stream_service_play_default(void);
/* 下一个频道，到底回环。这是**唯一的换台入口**：本地短语"换一个台"走它，云端
   self.radio.next 也走它。名字不参与选台——云端 ASR 把"音乐之声"切成"音乐之声儿"
   时，任何字符串匹配都只能靠猜，而猜错的代价是"用户说了没反应"。 */
esp_err_t radio_stream_service_play_next(void);
/* 当前频道名，只给日志和设备状态上报用，选台不看它。 */
const char *radio_stream_service_station_name(void);
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
/* 用户手动按下的暂停（“暂停电台”），不含被语音让位。和 is_active() 一起看才能
   分清"没在响"的两种原因：让位是语音回合在占用扬声器，暂停才是电台停住了。
   is_requested() 在暂停期间为假——它是"电台是不是当前模式"，不是"播放状态"。 */
bool radio_stream_service_is_user_paused(void);
/* 用户/云端是否要求电台在播：从 play_default()/play_next() 到 stop()/pause()
   之间为真，包含还在 CONNECTING、尚未出声、以及被语音让位的阶段。
   语音侧靠它判断“当前是电台模式”，设备状态上报也用它。 */
bool radio_stream_service_is_requested(void);

/* 读取并消费一次性电台失败状态，供下一次唤醒记录。 */
bool radio_stream_service_consume_failure(char *stage, size_t stage_size,
                                          int *error_code, uint8_t *reconnects);
