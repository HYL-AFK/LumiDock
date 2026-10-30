#include "thermal_service.h"

#include <math.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "fan_control.h"
#include "ntc_sensor.h"

static const char *TAG = "thermal_service";

/* 挡位分界：
 *   0  - 40 C   停转
 *   40 - 60 C   50%
 *   60 - 90 C   70%
 *   90 C 以上   100%
 *
 * 分界值本身是升档阈值：到 40 就上 50%，到 60 就上 70%，到 90 就上满。用户给的
 * 就是这几条线，不额外加余量，否则 40.0 C 的时候风扇不转，和需求差一截。 */
#define THERMAL_TIER_MID_C 40.0f
#define THERMAL_TIER_HIGH_C 60.0f
#define THERMAL_TIER_FULL_C 90.0f

#define THERMAL_TIER_MID_PERCENT 50U
#define THERMAL_TIER_HIGH_PERCENT 70U
#define THERMAL_TIER_FULL_PERCENT 100U

/* 退档阈值比进档低这么多。温度停在分界线上时读数会在 ±0.3 C 里来回抖，没有滞回
 * 风扇就会贴着 40 C 一秒一启停，比一直转着还吵。 */
#define THERMAL_HYSTERESIS_C 2.0f

/* 连续这么多次坏读数才认为探头真的坏了。2 次（2 秒）太短：NTC 分压在
 * 风扇 PWM 附近工作时，单组采样里偶尔会混进两个尖峰，一秒接着一秒地碰上
 * 就足以把一个好探头锁成全速。实测两分钟内误锁 3 次，风扇在 50% 和 100%
 * 之间来回跳。放宽到 5 次之后，真正的悬空依然在 5 秒内被发现（悬空时均值
 * 乱跳，几乎不可能连着 5 次都落在正常范围），而偶发干扰被过滤掉了。 */
#define THERMAL_PROBE_FAULT_SAMPLES 5U

/* 保护一旦进入，要连续这么多次可信读数才解除。放松这个门槛，保护就会被一次
 * 侥幸蒙混过关的伪读数撤销——实测过：悬空引脚偶尔能连续 8 次落在噪声谷底，
 * 于是"风扇已锁全速"下一轮就被改回 off，比一开始就不保护更糟。 */
#define THERMAL_RECOVERY_SAMPLES 5U

/* 10k/3950 的热敏电阻在这之外给出的阻值没有意义。落在这个区间外的读数只可能
 * 是分压算错了或者引脚上接的根本不是这颗探头。 */
#define THERMAL_PLAUSIBLE_MIN_C (-40.0f)
#define THERMAL_PLAUSIBLE_MAX_C 150.0f

/* 故障路径每秒被走到一次，日志要自己限速。 */
#define THERMAL_FAULT_LOG_INTERVAL_US (10LL * 1000LL * 1000LL)

/* 温度日志的限速：读数动了 0.1 C 就报一行，不动的话每分钟补一行心跳。
 * 标定探头时想看到的是每一次真实变化，但又不希望静止时刷屏。 */
#define THERMAL_LOG_DELTA_C 0.1f
#define THERMAL_LOG_HEARTBEAT_US (60LL * 1000LL * 1000LL)

typedef enum {
    THERMAL_TIER_OFF = 0,
    THERMAL_TIER_MID,
    THERMAL_TIER_HIGH,
    THERMAL_TIER_FULL,
} thermal_tier_t;

static bool s_ready;
static bool s_have_temperature;
static float s_temperature_c;
static thermal_tier_t s_tier;
static uint8_t s_fan_percent;
static unsigned s_probe_faults;
/* 风扇被锁到全速的保护态；只有连续几次可信读数才退出。 */
static bool s_protected;
static unsigned s_recovery_samples;
static int64_t s_fault_log_us;

static bool s_have_log;
static float s_last_log_c;
static int64_t s_last_log_us;

static const char *tier_name(thermal_tier_t tier)
{
    switch (tier) {
    case THERMAL_TIER_MID: return "mid";
    case THERMAL_TIER_HIGH: return "high";
    case THERMAL_TIER_FULL: return "full";
    case THERMAL_TIER_OFF:
    default: return "off";
    }
}

static uint8_t tier_percent(thermal_tier_t tier)
{
    switch (tier) {
    case THERMAL_TIER_MID: return THERMAL_TIER_MID_PERCENT;
    case THERMAL_TIER_HIGH: return THERMAL_TIER_HIGH_PERCENT;
    case THERMAL_TIER_FULL: return THERMAL_TIER_FULL_PERCENT;
    case THERMAL_TIER_OFF:
    default: return 0U;
    }
}

/* 进入该档需要的温度。 */
static float tier_rise_threshold(thermal_tier_t tier)
{
    switch (tier) {
    case THERMAL_TIER_MID: return THERMAL_TIER_MID_C;
    case THERMAL_TIER_HIGH: return THERMAL_TIER_HIGH_C;
    case THERMAL_TIER_FULL: return THERMAL_TIER_FULL_C;
    case THERMAL_TIER_OFF:
    default: return 0.0f;
    }
}

/* 退出该档回到下一档需要的温度，比进入它低 THERMAL_HYSTERESIS_C。 */
static float tier_fall_threshold(thermal_tier_t tier)
{
    return tier_rise_threshold(tier) - THERMAL_HYSTERESIS_C;
}

/* 一次可以跨多档。上电时机器可能已经很热，没必要从停转一档一档往上爬；
 * 两个方向各用各的阈值，所以跨档之后停在哪儿依然由滞回决定。 */
static thermal_tier_t tier_for_temperature(float celsius, thermal_tier_t current)
{
    thermal_tier_t tier = current;
    while (tier < THERMAL_TIER_FULL &&
           celsius >= tier_rise_threshold((thermal_tier_t)(tier + 1))) {
        tier = (thermal_tier_t)(tier + 1);
    }
    while (tier > THERMAL_TIER_OFF && celsius <= tier_fall_threshold(tier)) {
        tier = (thermal_tier_t)(tier - 1);
    }
    return tier;
}

static void apply_tier(thermal_tier_t tier)
{
    if (tier != s_tier) {
        ESP_LOGI(TAG, "fan %s -> %s at %.2f C: %u%%", tier_name(s_tier), tier_name(tier),
                 (double)s_temperature_c, (unsigned)tier_percent(tier));
        s_tier = tier;
    }
    s_fan_percent = tier_percent(tier);
    /* 驱动层自己会忽略重复值，所以这里不需要再确认一次是否变化。 */
    (void)fan_control_set_percent(s_fan_percent);
}

/* 逐次打印实测温度。这里报的是驱动刚转换出来的值，也就是控制实际用的那个值：
 * 标定探头的时候如果日志比控制值滞后，会让人以为探头慢半拍。
 *
 * 同时打出分压节点电压：万用表量到同一个数就说明 ADC 这一路没问题，温度还偏
 * 就只能是 Beta 参数或者分压电阻对不上，排查方向一眼能分开。 */
static void log_temperature(float celsius, int millivolts)
{
    const int64_t now_us = esp_timer_get_time();
    const bool moved = !s_have_log || fabsf(celsius - s_last_log_c) >= THERMAL_LOG_DELTA_C;
    const bool heartbeat = !s_have_log || (now_us - s_last_log_us) >= THERMAL_LOG_HEARTBEAT_US;
    if (!moved && !heartbeat) return;

    if (s_fan_percent > 0U) {
        /* 风扇转着的时候把实测转速一起带上。FG 没接、上拉缺失、或者风扇根本
         * 不支持测速，都会直接表现为 rpm 恒为 0——不把它打出来，这三种情况
         * 就只能靠猜。 */
        ESP_LOGI(TAG, "temp %.2f C | node %d mV | tier=%s fan=%u%% | %u rpm",
                 (double)celsius, millivolts, tier_name(s_tier),
                 (unsigned)s_fan_percent, (unsigned)thermal_service_get_fan_rpm());
    } else {
        ESP_LOGI(TAG, "temp %.2f C | node %d mV | tier=%s fan=%u%%", (double)celsius,
                 millivolts, tier_name(s_tier), (unsigned)s_fan_percent);
    }

    s_last_log_c = celsius;
    s_last_log_us = now_us;
    s_have_log = true;
}

static bool fault_log_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_fault_log_us < THERMAL_FAULT_LOG_INTERVAL_US) return false;
    s_fault_log_us = now_us;
    return true;
}

static void note_fault(void)
{
    /* 任何一次坏读数都把恢复计数清零：只有连续干净的一串才算真的恢复了。 */
    s_recovery_samples = 0U;
    if (++s_probe_faults < THERMAL_PROBE_FAULT_SAMPLES) return;
    if (s_protected) return;

    /* 一次性升级：从这一刻起风扇全速，直到连续几次读到可信温度。探头读不出来
     * 就等于板子不知道自己在多热，这时候停着风扇是错的失效方向。 */
    s_protected = true;
    ESP_LOGE(TAG, "NTC unusable for %u polls; fan locked at full speed", s_probe_faults);
    s_tier = THERMAL_TIER_FULL;
    s_fan_percent = THERMAL_TIER_FULL_PERCENT;
    (void)fan_control_set_percent(s_fan_percent);
}

static void note_valid(void)
{
    s_probe_faults = 0U;
    if (!s_protected) return;

    if (++s_recovery_samples < THERMAL_RECOVERY_SAMPLES) return;

    s_protected = false;
    s_recovery_samples = 0U;
    ESP_LOGI(TAG, "NTC recovered after %u consecutive good samples",
             (unsigned)THERMAL_RECOVERY_SAMPLES);
}

esp_err_t thermal_service_init(void)
{
    if (s_ready) return ESP_OK;

    s_temperature_c = NAN;
    s_have_temperature = false;
    s_tier = THERMAL_TIER_OFF;
    s_fan_percent = 0U;
    s_probe_faults = 0U;
    s_protected = false;
    s_recovery_samples = 0U;
    s_fault_log_us = 0;
    s_have_log = false;
    s_last_log_c = 0.0f;
    s_last_log_us = 0;

    /* NTC 失败不拦路：风扇驱动照常起来，只是 poll 会因为传感器没就绪而空转，
     * 日志里会留下明确原因。 */
    const esp_err_t ntc_err = ntc_sensor_init();
    if (ntc_err != ESP_OK) {
        ESP_LOGW(TAG, "NTC init failed: %s; no temperature control", esp_err_to_name(ntc_err));
    }

    const esp_err_t fan_err = fan_control_init();
    if (fan_err != ESP_OK) {
        ESP_LOGW(TAG, "fan init failed: %s; thermal control disabled", esp_err_to_name(fan_err));
        return fan_err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "thermal tiers: <%.0f C off | <%.0f C %u%% | <%.0f C %u%% | above %u%% "
                  "(hysteresis %.0f C)",
             (double)THERMAL_TIER_MID_C, (double)THERMAL_TIER_HIGH_C,
             (unsigned)THERMAL_TIER_MID_PERCENT, (double)THERMAL_TIER_FULL_C,
             (unsigned)THERMAL_TIER_HIGH_PERCENT, (unsigned)THERMAL_TIER_FULL_PERCENT,
             (double)THERMAL_HYSTERESIS_C);
    return ESP_OK;
}

void thermal_service_poll(void)
{
    if (!s_ready || !ntc_sensor_is_ready()) return;

    float celsius = 0.0f;
    int millivolts = 0;
    if (ntc_sensor_read_sample(&millivolts, &celsius) != ESP_OK) {
        note_fault();
        return;
    }

    /* 驱动已经筛掉了噪声散布和电源轨，这里再筛物理范围：超出这颗热敏电阻的
     * 工作区间，算出来的阻值就没有意义了。 */
    if (celsius < THERMAL_PLAUSIBLE_MIN_C || celsius > THERMAL_PLAUSIBLE_MAX_C) {
        if (fault_log_due()) {
            ESP_LOGW(TAG, "temperature %.2f C out of range; sample rejected", (double)celsius);
        }
        note_fault();
        return;
    }

    note_valid();
    s_temperature_c = celsius;
    s_have_temperature = true;

    /* 读数先打出来：恢复计数期间用户需要看到温度确实回来了。 */
    log_temperature(celsius, millivolts);

    /* 但档位不能跟着这几次读数走。恢复计数还没攒够，风扇就仍在保护态，这时候
     * 按温度把档位调下去，和"探头不可信所以全速"自相矛盾，而且万一下一次读数
     * 又是假的，风扇已经在低速上了。等 note_valid() 真正解除保护再说。 */
    if (s_protected) return;

    apply_tier(tier_for_temperature(celsius, s_tier));
}

float thermal_service_get_temperature(void)
{
    return s_have_temperature ? s_temperature_c : NAN;
}

uint8_t thermal_service_get_fan_percent(void)
{
    return s_fan_percent;
}

uint32_t thermal_service_get_fan_rpm(void)
{
    return fan_control_get_rpm();
}
