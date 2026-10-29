#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* 整点报时：7:00 到 21:00 之间的每一个整点，用本地 PCM 报一次时。
 *
 * 不接云端、不接语音服务：整点报时是背景性的，接 LLM 现场生成要么延迟到整点
 * 过了才响，要么在断网时彻底不响。素材是预录的 16 kHz mono s16le。 */

esp_err_t hourly_chime_service_init(void);

/* 每秒调用一次（snapshot_bridge）。内部按本地时间判断是否到点。 */
void hourly_chime_service_poll(void);

/* 已经报过的最后一个整点的小时数，没报过返回 -1。给后面的 UI 或日志用。 */
int hourly_chime_service_last_hour(void);
