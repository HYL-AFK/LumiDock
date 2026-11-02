#include "time_service.h"

#include <string.h>
#include <time.h>

#include "app_rtc.h"
#include "app_time.h"
#include "fault_registry.h"
#include "time_snapshot.h"
#include "mid_snapshot.h"
#include "watchdog_service.h"

/* 等级1 的复位请求只发一次。发出去之后看门狗要 8 秒才真正复位，这段时间
 * 里这个 1 Hz 拍子还会跑八圈，不复位标志的话会把串口刷成同一行的复读。 */
static bool s_fault_reset_armed;

esp_err_t time_service_init(void)
{
    esp_err_t err = time_snapshot_init();
    if (err != ESP_OK) return err;
    return app_time_init();
}

void time_service_poll(void)
{
    /* DS3231 探不到时的重试挂在这个 1 Hz 拍子上，省下 WiTime 那边一条
     * 3072 字节栈的 rtc_task。探到之后这里就是空转。 */
    app_rtc_poll();

    /* 等级1 故障的兜底窗口。放在这个拍子上，是因为它是全系统唯一一个
     * 既跑在稳定任务栈上、又不依赖网络和业务状态的周期节拍——看门狗自己
     * 的任务只能看心跳是否迟到，看不出“某个致命功能已经半小时没回来”。
     *
     * 不在这里点故障灯：status_led 的 choose_state 每 50 ms 读一次
     * fault_registry_highest_level()，等级1 一挂上它自己就是红灯。重复
     * 调 set_critical_fault 反而会把 P101 混进真正的故障码里。 */
    if (!s_fault_reset_armed && fault_registry_level1_overdue()) {
        s_fault_reset_armed = true;
        (void)watchdog_service_request_reset("level1 fault grace window expired");
    }

    if (!time_snapshot_is_ready()) return;
    lumidock_snapshot_t current;
    mid_snapshot_get(&current);
    time_weather_snapshot_t value = current.time_weather;
    const time_t now = time(NULL);
    value.valid = true;
    localtime_r(&now, &value.local_time);
    (void)strlcpy(value.timezone, "CST-8", sizeof(value.timezone));
    mid_snapshot_set_time_weather(&value);
}

void time_service_notify_network(void)
{
    app_time_notify_network();
}

void time_service_notify_internet_online(void)
{
    app_time_notify_internet_online();
}

void time_service_apply_http_time(time_t utc_time)
{
    app_time_apply_http_time(utc_time);
}

bool time_service_is_initial_location_settled(void)
{
    return app_time_is_initial_location_settled();
}
