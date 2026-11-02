#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 故障码寄存器。
 *
 * 和 runtime_event_log 的分工：那边是流水账（按时间排的事件），这边是状态表
 * （此刻有哪些故障挂着）。同一个故障不会因为反复发生就把流水挤爆，表里永远
 * 只有一行，count 记次数。
 *
 * 编码是 P + 等级 + 两位序号，等级直接体现在码值里，串口看到 P2xx 就知道是
 * 降级类，不用回头查表。
 *
 * 等级语义：
 *   1xx  致命   核心能力没了。挂载满 30 分钟仍未恢复就主动重启——重启是最后
 *               手段，但连续半小时都救不回来，说明只剩这一条路。
 *   2xx  降级   功能缺失但设备可用，可以永久挂载。低电量、断网都属于这类：
 *               重启解决不了问题，反而会把用户好不容易建立起来的会话清掉。
 *   3xx  诊断   只记进表里供查询，不改变任何行为。
 *
 * 灯效对所有等级都有反馈，强度按等级递减，具体映射见 status_led_service.c。
 *
 * 新增码时只能往各段末尾追加，不能重排——NVS 里存的是码值，重排会让旧记录
 * 指向另一个故障。 */
typedef enum {
    /* ---- 等级1：核心能力丧失，超时重启 ---- */
    FAULT_P101_WATCHDOG_LATCHED = 0x101,
    FAULT_P102_SNAPSHOT_STALLED = 0x102,
    FAULT_P103_DISPLAY_LOST = 0x103,
    FAULT_P104_AUDIO_CODEC_LOST = 0x104,
    FAULT_P105_HEAP_CRITICAL = 0x105,

    /* ---- 等级2：降级可用，永久挂载 ---- */
    FAULT_P201_LOW_BATTERY = 0x201,
    FAULT_P202_WIFI_LOST = 0x202,
    FAULT_P203_INTERNET_DOWN = 0x203,
    FAULT_P204_TIME_UNSYNCED = 0x204,
    FAULT_P205_BMS_OFFLINE = 0x205,
    FAULT_P206_RTC_ABSENT = 0x206,
    FAULT_P207_THERMAL_HIGH = 0x207,
    FAULT_P208_STC_OFFLINE = 0x208,

    /* ---- 等级3：只记录，不处置 ---- */
    FAULT_P301_WEATHER_STALE = 0x301,
    FAULT_P302_NEWS_FAILED = 0x302,
    FAULT_P303_RADIO_RECONNECTING = 0x303,
    FAULT_P304_VOICE_DEGRADED = 0x304,
    FAULT_P305_TARIFF_STALE = 0x305,
} fault_code_t;

typedef enum {
    FAULT_LEVEL_NONE = 0,
    FAULT_LEVEL_1 = 1,
    FAULT_LEVEL_2 = 2,
    FAULT_LEVEL_3 = 3,
} fault_level_t;

/* 等级1 挂载多久还没恢复就重启。 */
#define FAULT_LEVEL1_REBOOT_TIMEOUT_S (30U * 60U)

/* 表容量。当前定义了 18 个码，留出余量给后续扩展。满了之后新故障会顶掉
 * 最老的已恢复记录，仍在挂载的记录不会被顶掉。 */
#define FAULT_REGISTRY_CAPACITY 24

esp_err_t fault_registry_init(void);

/* 挂载一个故障。重复调用只累加 count，不会产生新行。
 * detail 仅进日志，用于留存现场数值（电量百分比、错误码等）。 */
esp_err_t fault_registry_raise(fault_code_t code, int32_t detail);

/* 消除一个故障。code 未挂载时静默返回 ESP_OK——恢复路径本来就可能被多走
 * 一遍（比如网络断开和连通的事件都会触发状态刷新）。 */
esp_err_t fault_registry_clear(fault_code_t code);

bool fault_registry_is_active(fault_code_t code);

/* 当前挂载的最高等级，0 表示没有故障。灯效和 UI 都按这个值选状态。 */
fault_level_t fault_registry_highest_level(void);

/* 当前挂载的故障条数。 */
uint16_t fault_registry_active_count(void);

/* 按码值取等级。供 NVS 加载时校验用。 */
fault_level_t fault_code_level(uint16_t code);

/* 码值转 "P201_LOW_BATTERY" 形式，未知码返回 "P???"。
 * 带语义名而不是只有数字，是因为串口日志和故障表转储里光看四位数字对不上
 * 具体是什么故障。返回值指向静态常量，不需要释放。 */
const char *fault_code_name(uint16_t code);

/* 等级1 超时判定，由 time_service_poll() 那个 1 Hz 拍子调用。
 * 返回 true 表示该重启了：调用方负责先点 critical_fault 灯效，再停喂狗。 */
bool fault_registry_level1_overdue(void);

/* 清空整张表，含 NVS。仅供诊断和恢复出厂使用。 */
esp_err_t fault_registry_clear_all(void);

/* 把当前表打到串口，开机时调用一次方便现场倒查。 */
void fault_registry_dump(void);
