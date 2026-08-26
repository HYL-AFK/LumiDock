#pragma once

/*
 * 统一日志封装。
 *
 * 目的：
 * 1. 日志里固定带文件、行号、函数名，方便快速定位问题。
 * 2. 保持底层仍然走 ESP-IDF 日志系统，后续如果要接 BLE/UI/持久化日志，
 *    只需要改这一层，不需要全项目到处改调用点。
 */

#include "esp_err.h"
#include "esp_log.h"

#ifndef APP_LOG_FILE
#if defined(__FILE_NAME__)
#define APP_LOG_FILE __FILE_NAME__
#else
#define APP_LOG_FILE __FILE__
#endif
#endif

#define APP_LOG_FMT(fmt) "[%s:%d:%s] " fmt, APP_LOG_FILE, __LINE__, __func__

#define APP_LOGE(tag, fmt, ...) ESP_LOGE(tag, APP_LOG_FMT(fmt), ##__VA_ARGS__)
#define APP_LOGW(tag, fmt, ...) ESP_LOGW(tag, APP_LOG_FMT(fmt), ##__VA_ARGS__)
#define APP_LOGI(tag, fmt, ...) ESP_LOGI(tag, APP_LOG_FMT(fmt), ##__VA_ARGS__)
#define APP_LOGD(tag, fmt, ...) ESP_LOGD(tag, APP_LOG_FMT(fmt), ##__VA_ARGS__)

#define APP_LOG_ERR(tag, err, fmt, ...) \
    ESP_LOGE(tag, APP_LOG_FMT(fmt " | err=%s(0x%x)"), ##__VA_ARGS__, esp_err_to_name(err), (unsigned int)(err))

#define APP_LOG_STATE(tag, fmt, ...) ESP_LOGI(tag, APP_LOG_FMT(fmt), ##__VA_ARGS__)

#define APP_LOG_HEX(tag, title, buf, len)            \
    do {                                             \
        ESP_LOGI(tag, APP_LOG_FMT("%s len=%u"),      \
                 title, (unsigned int)(len));        \
        ESP_LOG_BUFFER_HEX_LEVEL(tag, buf, len, ESP_LOG_INFO); \
    } while (0)
