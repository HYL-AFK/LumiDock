#include "app_rtc.h"

#include "board_pins.h"

#if !LUMIDOCK_DS3231_ENABLE

/* 模块还没接，只占着引脚。三个入口都退化成空实现，不发 I2C、不碰系统时钟、
 * 也不占日志。开关定义在 board_pins.h 的 LUMIDOCK_DS3231_ENABLE。 */
esp_err_t app_rtc_init(void) { return ESP_OK; }
void app_rtc_poll(void) {}
void app_rtc_sync_from_system(void) {}

#else

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "esp_timer.h"

#include "app_log.h"
#include "ds3231.h"
#include "fault_registry.h"
#include "time_snapshot.h"

/* 开机时没探到芯片的重试节奏：每 5 秒一次，最多 12 次（约 1 分钟）。
 * 之后就不再试了——芯片没焊或者坏了，每秒刷一条日志只会把串口淹掉。
 * 校时成功后还会再试一次（app_rtc_sync_from_system），不受这个上限约束。 */
#define RTC_RETRY_INTERVAL_MS 5000
#define RTC_RETRY_MAX_ATTEMPTS 12U

/* 两次写回之间的最小间隔。SNTP 一次开机只同步一回，但 HTTP Date 头可能连着
 * 来好几发，没必要每次都往芯片里写一遍。 */
#define RTC_WRITEBACK_MIN_INTERVAL_S 5

static const char *TAG = "app_rtc";

static bool s_ready;
static volatile bool s_writeback_pending;
static uint32_t s_retry_attempts;
static int64_t s_next_retry_us;
static time_t s_last_writeback;

static void schedule_retry(void)
{
    s_next_retry_us = esp_timer_get_time() + (int64_t)RTC_RETRY_INTERVAL_MS * 1000;
}

static void log_time(const char *prefix, const struct tm *value)
{
    APP_LOGI(TAG, "%s %04d-%02d-%02d %02d:%02d:%02d UTC", prefix,
             value->tm_year + 1900, value->tm_mon + 1, value->tm_mday,
             value->tm_hour, value->tm_min, value->tm_sec);
}

/* 把芯片里的 UTC 灌进系统时钟和快照。时间不可信时宁可不动，理由见下面。 */
static bool apply_rtc_time(void)
{
    ds3231_snapshot_t snap = {0};
    const esp_err_t err = ds3231_read(&snap);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "RTC read failed");
        return false;
    }

    if (!snap.time_valid) {
        /* OSF 置位说明振荡器停过：电池没电、首次上电、或者芯片被整段断电。
         * 这种情况下寄存器里是停振那一刻的残留值，出厂态是 2000-01-01
         * 00:00:00。绝对不能拿它去 settimeofday——时间倒退十几年，TLS 证书
         * 会全部判为"尚未生效"，定位和天气两个 https 请求直接瘫掉，而它们又
         * 是 SNTP 之外唯一能得到正确时间的途径。等校时写回之后自然就对了。 */
        APP_LOGW(TAG, "RTC time not trustworthy, keeping system clock: osf=%u",
                 (unsigned)((snap.status & 0x80U) != 0U));
        log_time("RTC raw value:", &snap.utc);
        return false;
    }

    const time_t unix_time = ds3231_utc_to_unix(&snap.utc);
    if (unix_time <= 0) {
        APP_LOGW(TAG, "RTC time conversion produced an invalid timestamp");
        return false;
    }

    const struct timeval tv = {.tv_sec = unix_time, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) {
        APP_LOGW(TAG, "settimeofday from RTC failed");
        return false;
    }

    time_snapshot_t snapshot = {0};
    localtime_r(&unix_time, &snapshot.local_time);
    snapshot.unix_time = unix_time;
    snapshot.from_rtc = true;
    snapshot.from_sntp = false;
    time_snapshot_set(&snapshot);

    APP_LOGI(TAG, "system clock restored from RTC: %04d-%02d-%02d %02d:%02d:%02d",
             snapshot.local_time.tm_year + 1900, snapshot.local_time.tm_mon + 1,
             snapshot.local_time.tm_mday, snapshot.local_time.tm_hour,
             snapshot.local_time.tm_min, snapshot.local_time.tm_sec);
    return true;
}

/* 把系统时钟写回芯片。只在 app_rtc_poll() 里调，也就是 snapshot_bridge
 * 那个 1 Hz 拍子的上下文。 */
static void write_back_system_clock(void)
{
    const time_t now = time(NULL);
    if (now <= 0) return;
    if (s_last_writeback > 0 && now >= s_last_writeback &&
        now - s_last_writeback < RTC_WRITEBACK_MIN_INTERVAL_S) {
        return;
    }

    struct tm utc = {0};
    gmtime_r(&now, &utc);
    const esp_err_t err = ds3231_write_utc(&utc);
    if (err != ESP_OK) {
        APP_LOG_ERR(TAG, err, "RTC writeback failed");
        return;
    }
    s_last_writeback = now;
    log_time("RTC writeback OK:", &utc);
}

esp_err_t app_rtc_init(void)
{
    /* 幂等：time_service_init() 已经建过一次，这里只是防止调用顺序变化。 */
    (void)time_snapshot_init();

    /* 这里一行 I2C 都不碰。调用链是 app_main → app_manager_start() →
     * time_service_init() → app_time_init() → 这里，而 app_main 的任务栈只有
     * 3584 字节，前面已经叠了全套驱动初始化。ds3231_init() 首次要过
     * i2c_new_master_bus()（建总线、分配队列和中断）再加两行格式化日志，
     * 在这个位置跑就是拿边界赌运气。
     *
     * 统一留给 app_rtc_poll()：它跑在 snapshot_bridge 的 5120 字节 PSRAM 栈上，
     * 而那个任务在 app_manager_start() 末尾创建，比本函数晚，所以这里是
     * 真的“排到下一拍”。代价是取 RTC 时间比原先晚不到 1 秒，而 SNTP 本来
     * 也要开机后好几秒才开始。 */
    s_next_retry_us = 0;
    return ESP_OK;
}

void app_rtc_poll(void)
{
    if (!s_ready) {
        if (s_retry_attempts >= RTC_RETRY_MAX_ATTEMPTS) {
            /* 重试次数耗尽，芯片确实不在。只在用尽的那一刻挂码，不是每次
             * 跳过都挂——这个分支每次轮询都会进来。
             *
             * 降级而不是致命：没有 RTC 只是每次冷开机要等 SNTP 才有准时间，
             * 设备照常能用，不重启也恢复不了（模块根本没接）。 */
            (void)fault_registry_raise(FAULT_P206_RTC_ABSENT, 0);
            return;
        }
        if (esp_timer_get_time() < s_next_retry_us) return;

        ++s_retry_attempts;
        if (ds3231_init() != ESP_OK) {
            schedule_retry();
            return;
        }
        APP_LOGI(TAG, "RTC detected on retry %u", (unsigned)s_retry_attempts);
        s_ready = true;
        (void)fault_registry_clear(FAULT_P206_RTC_ABSENT);
        (void)apply_rtc_time();
    }

    /* 已就绪时这里不做周期读。系统时间由 esp_timer/内部 RTC 自己走，每秒再从
     * 芯片读一遍既不必要，也会让时间被两套来源来回拉扯。只有校时那边挂了
     * 待写回时，这里才真的下一趟 I2C。 */
    if (!s_writeback_pending) return;
    s_writeback_pending = false;
    write_back_system_clock();
}

void app_rtc_sync_from_system(void)
{
    /* 这里只置标志，真正的 I2C 事务留给 app_rtc_poll()。
     *
     * 两个调用方都跑在别人的任务栈上：HTTP Date 来自 internet_probe（栈
     * 3072 字节，esp_http_client_perform() 已经吃掉大半），SNTP 来自 app_time
     * 自己的同步循环。在这个位置同步调 ds3231_init()——里面是
     * i2c_new_master_bus()/i2c_master_bus_add_device() 加两行带格式化的
     * ESP_LOGW——实测就是在 "HTTP Date time applied" 同一毫秒里报
     * "A stack overflow in task internet_probe" 并重启。
     *
     * 换成标志位之后，I2C 只会在 snapshot_bridge 那个 1 Hz 拍子上发生。 */
    s_writeback_pending = true;

    /* 顺带把下一次探测提前：芯片可能上电比 ESP32 慢，开机那会儿还没起来，而
     * 校时通常发生在开机后 7~10 秒，正好落在重试窗口里。这里只是免掉 5 秒
     * 等待，不突破 RTC_RETRY_MAX_ATTEMPTS 的上限，所以不会把串口刷满。 */
    s_next_retry_us = 0;
}

#endif /* LUMIDOCK_DS3231_ENABLE */
