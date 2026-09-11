#pragma once

#include <stdbool.h>

/*
 * 需要在重启后保留的用户设置。
 *
 * 目前只有扬声器音量。模块自己持有一个内部 RAM 栈的小任务，因为写 NVS 会
 * 关闭 SPI Cache，运行在 PSRAM 栈上的 task（语音会话、MCP 处理）直接写会
 * 触发 esp_task_stack_is_sane_cache_disabled() 断言。
 */

/* 创建持久化任务并读取已保存的音量。只调用一次，且调用方的任务栈必须在
 * 内部 RAM 中（app_manager 的主任务满足这一点）。 */
void device_settings_service_init(void);

/* 把已保存的音量应用到 ES8311。返回 true 表示存在已保存值并被应用，
 * false 表示保留编解码器上电默认值。必须在 i2s_audio_driver_init() 成功
 * 之后调用。 */
bool device_settings_service_apply_saved_volume(void);

/* 请求持久化 0..100 的音量。非阻塞，可从任意任务调用；连续多次调用会被
 * 合并成一次 NVS 写入。 */
void device_settings_service_request_volume_save(int percent);
