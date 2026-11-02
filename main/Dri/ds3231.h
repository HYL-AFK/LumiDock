#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

/* DS3231 走独立 I2C 总线（引脚见 board_pins.h 的 LUMIDOCK_DS3231_*）。
 *
 * 这里存的是 **UTC**，不是本地时间。移植自 D:\Project\WiTime 的同名驱动，但那
 * 一版存的是本地时间——它时区固定，所以没暴露问题。LumiDock 不一样：
 * app_time.c 会根据定位结果动态改 TZ（app_time_apply_timezone_offset），本地
 * 时间会跟着变，UTC 不会。存本地时间的话，用户换个时区，芯片里那串数字就和
 * 系统时间对不上了。 */

typedef struct {
    /* 还原出来的 UTC 时间。读取成功即填好，但可靠性看 time_valid。 */
    struct tm utc;
    /* 状态寄存器 0x0F 原值。bit7=OSF(振荡器停振标志)，bit2=BSY，bit0/1=闹钟。 */
    uint8_t status;
    /* 时间是否可信：OSF 为 0（上电后没停过振）且年份落在合理区间。
     * 电池没电或者首次上电时 OSF=1，寄存器里是 2000-01-01 这种出厂值——
     * 这种时间绝不能拿去 settimeofday()，否则 TLS 证书校验会全线失败，
     * 定位和天气都跟着停摆。 */
    bool time_valid;
} ds3231_snapshot_t;

/* 建总线、挂设备、验证一次真实读写。可重复调用：失败后下次调用只重试探测，
 * 不会重复 i2c_new_master_bus()。 */
esp_err_t ds3231_init(void);

/* 总线已建好且芯片应答过。 */
bool ds3231_is_ready(void);

/* 读时间寄存器 + 状态寄存器，一次性填好快照。 */
esp_err_t ds3231_read(ds3231_snapshot_t *out);

/* 把 UTC 写入芯片，并清掉 OSF。year 允许 2000~2199。 */
esp_err_t ds3231_write_utc(const struct tm *utc);

/* struct tm(UTC) -> Unix 秒。工具函数，独立于 I2C，测试里直接用。
 * 不用 timegm()：newlib 没声明它，而 mktime() 会按当前 TZ 解释，两者都不能用。 */
time_t ds3231_utc_to_unix(const struct tm *utc);
