#pragma once

#include "esp_err.h"

/* DS3231 接入。移植自 D:\Project\WiTime 的 app_rtc，但做了三处改动：
 *
 * 1. 不建任务。WiTime 那边是 xTaskCreate(rtc_task, 3072)，3072 字节栈必然落在
 *    内部 RAM，而这块板子的内部 RAM 一直紧张——之前 websocket 任务申请 4096
 *    字节连续内部内存都拿不到，直接导致语音唤醒没反应。这里的读取挂在
 *    time_service_poll() 那个 1 Hz 的拍子上，一个字节都不用多花。
 * 2. 不读芯片温度。WiTime 拿它当天气温度源，LumiDock 的温度走 NTC、天气温度走
 *    网络，两条线都不该被 RTC 顶掉。
 * 3. 存 UTC 而不是本地时间。WiTime 时区固定，存本地还看不出来；LumiDock 会根据
 *    定位结果动态改 TZ，存本地时间的话换个时区芯片里那串数就和系统对不上了。 */

/* 登记 RTC 接入。成功返回只代表“已排进 app_rtc_poll() 的第一拍”，
 * 不代表芯片已经应答——真正的探测和读取都在那个 1 Hz 拍子上做，因为
 * 调用方 app_main 只有 3584 字节栈，塞不下 i2c_new_master_bus()。
 * 芯片不在也不致命：没有 RTC 也能靠 SNTP 走。 */
esp_err_t app_rtc_init(void);

/* 1 Hz 调用。负责两件事：开机时没探到芯片就重试若干次，以及执行
 * app_rtc_sync_from_system() 挂下的写回。两者都不必要时是空转。 */
void app_rtc_poll(void);

/* 系统时间被 SNTP / HTTP Date 校准之后调用，把 UTC 写回芯片供下次冷启动用。
 *
 * 这里只置一个待写标志，真正的 I2C 事务在下一拍 app_rtc_poll() 里做。两个
 * 调用点所在的上下文（internet_probe 任务 3072 字节栈、app_time 同步循环）
 * 都塞不下 i2c_new_master_bus()，同步调会直接栈溢出重启。 */
void app_rtc_sync_from_system(void);
