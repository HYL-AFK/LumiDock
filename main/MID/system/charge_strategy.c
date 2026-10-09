#include "charge_strategy.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#include "bms_service.h"
#include "tariff_service.h"
#include "weather_snapshot.h"

static const char *const TAG = "charge";

/* ---- 判定门限。都是"改一行就能调"的参数，不要散进逻辑里。 ---- */
/* 0°C 以下充电会在负极析锂，是真正伤电池的区间，不是保守取整。 */
#define CHARGE_TEMP_MIN_C 0
#define CHARGE_TEMP_MAX_C 45
/* 低于这个温度建议直接充满：低温续航打折，留余量比省那点电费重要。 */
#define CHARGE_TEMP_COLD_C 5
/* 留作余量的电量，不参与"够不够用"的估算——让用户把车推到 0% 不是策略。 */
#define CHARGE_RESERVE_SOC 15U
/* 省不到这么多（ct/kWh）就不值得让用户等，直接建议现在充。 */
#define CHARGE_SAVING_MIN_CT 3.0f
#define CHARGE_SECONDS_PER_HOUR 3600LL

static charge_config_t s_config = {
    /* 设备没有里程计，这两个是固定值。wh_per_km 的算法：
     * 电池总能量(Wh) ÷ 实际满电续航(km)。 */
    .daily_km = 15.0f,
    .wh_per_km = 25.0f,
    .charge_power_w = 150.0f,
    .target_soc = 80U,
    .min_soc = 30U,
    .departure_hour = 7U,
    .departure_minute = 30U,
};

/* 低温掉电快、雨天滚阻大：同样一段路，冷天能多吃掉四成的电。
 * 这个系数直接决定"要不要提前补电"，取保守值。 */
static float usage_factor(int8_t tomorrow_min_c, uint8_t tomorrow_precip)
{
    float factor = 1.0f;
    if (tomorrow_min_c < 0) factor = 1.4f;
    else if (tomorrow_min_c < 10) factor = 1.2f;
    else if (tomorrow_min_c > 30) factor = 1.05f;
    if (tomorrow_precip >= 60U) factor *= 1.1f;
    return factor;
}

/* 把时间戳说成"今天/明天 HH:MM"，直接进播报文案。 */
static void format_when(int64_t ts, char *out, size_t out_size)
{
    const time_t now = time(NULL);
    const time_t when = (time_t)ts;
    struct tm now_tm = {0};
    struct tm when_tm = {0};
    localtime_r(&now, &now_tm);
    localtime_r(&when, &when_tm);
    const int day_delta = when_tm.tm_yday - now_tm.tm_yday;
    if (day_delta == 1)
        snprintf(out, out_size, "明天 %02d:%02d", when_tm.tm_hour, when_tm.tm_min);
    else
        snprintf(out, out_size, "%02d:%02d", when_tm.tm_hour, when_tm.tm_min);
}

/* GCC 的 -Wformat-truncation 会把每个 %.Nf 都按 DBL_MAX 去估上界（约 310 字符），
 * 格式串里只要出现浮点，静态分析就认定必然截断——实测把一个 128 字节的目标算成
 * "可能输出 1482 字节"，-Werror 直接把构建打挂。而这几个量都是小范围物理值，
 * 先转成定点整数再打印：编译器算得出范围，播报文本也能短一截。 */
static void format_ct(char *out, size_t out_size, float value)
{
    const int scaled = (int)(value * 10.0f + (value >= 0.0f ? 0.5f : -0.5f));
    const int magnitude = scaled < 0 ? -scaled : scaled;
    /* 德国电价会走到负值，符号位不能省。 */
    snprintf(out, out_size, "%s%d.%d", scaled < 0 ? "-" : "", magnitude / 10, magnitude % 10);
}

static void format_yuan(char *out, size_t out_size, float value)
{
    const int scaled = (int)(value * 100.0f + (value >= 0.0f ? 0.5f : -0.5f));
    const int magnitude = scaled < 0 ? -scaled : scaled;
    snprintf(out, out_size, "%s%d.%02d", scaled < 0 ? "-" : "", magnitude / 100, magnitude % 100);
}

static void format_km(char *out, size_t out_size, float value)
{
    snprintf(out, out_size, "%d", (int)(value + 0.5f));
}

void charge_strategy_init(void)
{
    ESP_LOGI(TAG, "充电策略就绪：日均 %.1f km，电耗 %.1f Wh/km，目标 %u%%，出发 %02u:%02u",
             (double)s_config.daily_km, (double)s_config.wh_per_km,
             (unsigned)s_config.target_soc, (unsigned)s_config.departure_hour,
             (unsigned)s_config.departure_minute);
}

void charge_strategy_get_config(charge_config_t *out)
{
    if (out == NULL) return;
    *out = s_config;
}

void charge_strategy_set_config(const charge_config_t *cfg)
{
    if (cfg == NULL) return;
    charge_config_t next = *cfg;
    /* 只做范围钳制，不替用户"纠正"意图。 */
    if (next.daily_km <= 0.0f || next.daily_km > 500.0f) next.daily_km = s_config.daily_km;
    if (next.wh_per_km <= 0.0f || next.wh_per_km > 200.0f) next.wh_per_km = s_config.wh_per_km;
    if (next.charge_power_w < 20.0f || next.charge_power_w > 3000.0f) next.charge_power_w = s_config.charge_power_w;
    if (next.target_soc < 50U || next.target_soc > 100U) next.target_soc = s_config.target_soc;
    if (next.min_soc > 80U) next.min_soc = s_config.min_soc;
    if (next.departure_hour > 23U) next.departure_hour = s_config.departure_hour;
    if (next.departure_minute > 59U) next.departure_minute = s_config.departure_minute;
    s_config = next;
}

const char *charge_strategy_action_name(charge_plan_action_t action)
{
    switch (action) {
    case CHARGE_PLAN_BLOCKED: return "blocked";
    case CHARGE_PLAN_NO_NEED: return "no_need";
    case CHARGE_PLAN_CHARGE_NOW: return "charge_now";
    case CHARGE_PLAN_DEFER: return "defer";
    default: return "unknown";
    }
}

bool charge_strategy_build_plan(charge_plan_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));

    bms_info_t bms;
    if (!bms_service_get_info(&bms) || !bms.online || !bms.parsed_valid) {
        ESP_LOGW(TAG, "电池数据不可用，不给充电建议");
        return false;
    }

    const uint8_t soc = bms.rsoc_percent;
    out->current_soc = soc;
    /* 取最热的那一路：充电时最热的电芯才是限制条件，不是平均值。 */
    int16_t hottest = bms.temp_c[0];
    for (uint32_t i = 1U; i < BMS_TEMP_COUNT; ++i) {
        if (bms.temp_c[i] > hottest) hottest = bms.temp_c[i];
    }
    out->battery_temp_c = hottest;

    /* 总能量用实测电压 × 满充容量算，比拿标称值推准。
     * pack_mv(mV)/1000 = V，V × mAh = mWh，再 /1000 = Wh。 */
    const float pack_v = (float)bms.pack_mv / 1000.0f;
    const float full_wh = pack_v * (float)bms.full_charge_capacity_mah / 1000.0f;
    if (full_wh < 1.0f) {
        ESP_LOGW(TAG, "可用容量异常（%.1f Wh），不给建议", (double)full_wh);
        return false;
    }

    /* ---- 第一层：安全。这一层过了才谈省不省钱。 ---- */
    if (hottest < CHARGE_TEMP_MIN_C) {
        out->action = CHARGE_PLAN_BLOCKED;
        snprintf(out->warning, sizeof(out->warning),
                 "电池 %d°C 低于 %d°C，现在充电会在负极析锂、伤电池，等回暖再充。",
                 (int)hottest, CHARGE_TEMP_MIN_C);
        snprintf(out->reason, sizeof(out->reason),
                 "当前电量 %u%%，但电池温度过低，暂时不建议充电。", (unsigned)soc);
        return true;
    }
    if (hottest > CHARGE_TEMP_MAX_C) {
        out->action = CHARGE_PLAN_BLOCKED;
        snprintf(out->warning, sizeof(out->warning),
                 "电池 %d°C 高于 %d°C，充电会加剧发热，先停一会儿再充。",
                 (int)hottest, CHARGE_TEMP_MAX_C);
        snprintf(out->reason, sizeof(out->reason),
                 "当前电量 %u%%，电池温度偏高，建议冷却后再充。", (unsigned)soc);
        return true;
    }

    /* ---- 第二层：够不够用。 ---- */
    weather_snapshot_t weather = {0};
    /* weather_snapshot_get 返回 void，不是 bool：它只负责把当前快照拷出来，
       有没有预报得看结构体里的 forecast_valid。 */
    weather_snapshot_get(&weather);
    const bool has_forecast = weather.forecast_valid;
    float factor = 1.0f;
    if (has_forecast) factor = usage_factor(weather.tomorrow_temp_min_c, weather.tomorrow_precip_prob);
    const float need_wh = s_config.daily_km * s_config.wh_per_km * factor;

    const float reserve_wh = full_wh * (float)CHARGE_RESERVE_SOC / 100.0f;
    const float current_wh = full_wh * (float)soc / 100.0f;
    const float usable_wh = current_wh > reserve_wh ? current_wh - reserve_wh : 0.0f;
    const bool below_floor = soc <= s_config.min_soc;
    const float deficit_wh = need_wh - usable_wh;

    if (!below_floor && deficit_wh <= 0.0f) {
        out->action = CHARGE_PLAN_NO_NEED;
        snprintf(out->reason, sizeof(out->reason),
                 "当前电量 %u%%，够明天大约 %d 公里，不用充。",
                 (unsigned)soc, (int)(s_config.daily_km + 0.5f));
        return true;
    }
    out->should_charge = true;

    /* ---- 第三层：充到多少。 ---- */
    uint8_t target = s_config.target_soc;
    /* 明天要吃掉大半块电池，或者天冷续航打折，就别只充到 80% 了。 */
    if (need_wh > full_wh * 0.5f || (has_forecast && weather.tomorrow_temp_min_c < CHARGE_TEMP_COLD_C))
        target = 100U;
    if (target <= soc) target = soc < 100U ? (uint8_t)(soc + 1U) : 100U;
    out->target_soc = target;

    const float add_wh = full_wh * (float)(target - soc) / 100.0f;
    out->energy_kwh = add_wh / 1000.0f;
    const float minutes_f = add_wh / s_config.charge_power_w * 60.0f;
    out->charge_minutes = minutes_f > 65535.0f ? 65535U : (uint16_t)minutes_f;

    /* ---- 第四层：什么时候充最划算。 ---- */
    tariff_snapshot_t tariff;
    const bool has_tariff = tariff_service_get(&tariff);
    if (has_tariff) {
        /* 电价单位是 ct/kWh，换算成"元"要再除 100。 */
        out->cost_est = out->energy_kwh * tariff.current_ct / 100.0f;
    }

    uint8_t hours = (uint8_t)((out->charge_minutes + 59U) / 60U);
    if (hours == 0U) hours = 1U;
    int64_t cheap_start = 0;
    float cheap_ct = 0.0f;
    const bool have_window =
        has_tariff && tariff_service_find_cheapest(&tariff, hours, &cheap_start, &cheap_ct);
    /* 窗口必须还没开始，否则它已经在跑了，等于"现在充"。 */
    const bool window_ahead = have_window && cheap_start > (int64_t)time(NULL);

    char when[32] = {0};
    char caveat[64] = {0};
    char km_text[32] = {0};
    char now_text[32] = {0};
    char cheap_text[32] = {0};
    char save_text[32] = {0};
    /* factor 是"耗电放大倍数"，说成续航折扣要取倒数：1.2 倍耗电 = 8.3 折续航。 */
    if (has_forecast && factor > 1.0f) {
        snprintf(caveat, sizeof(caveat), "（明天低温/有雨，续航约打 %d 折）",
                 (int)(10.0 / (double)factor + 0.5));
    }

    if (below_floor) {
        /* 已经踩到警戒线：先充上，不为省几毛钱让用户第二天推车。 */
        out->action = CHARGE_PLAN_CHARGE_NOW;
        snprintf(out->reason, sizeof(out->reason),
                 "电量只剩 %u%%，建议现在就充到 %u%%，大约需要 %u 分钟%s。",
                 (unsigned)soc, (unsigned)target, (unsigned)out->charge_minutes, caveat);
    } else if (window_ahead && cheap_ct + CHARGE_SAVING_MIN_CT < tariff.current_ct) {
        format_when(cheap_start, when, sizeof(when));
        out->action = CHARGE_PLAN_DEFER;
        out->start_ts = cheap_start;
        out->end_ts = cheap_start + (int64_t)hours * CHARGE_SECONDS_PER_HOUR;
        out->saving_est = out->energy_kwh * (tariff.current_ct - cheap_ct) / 100.0f;
        format_km(km_text, sizeof(km_text), s_config.daily_km);
        format_ct(now_text, sizeof(now_text), tariff.current_ct);
        format_ct(cheap_text, sizeof(cheap_text), cheap_ct);
        format_yuan(save_text, sizeof(save_text), out->saving_est);
        snprintf(out->reason, sizeof(out->reason),
                 "明天大约骑 %s 公里%s，现在电价 %s 分/度，%s 起只要 %s 分/度，"
                 "建议等到那时候充到 %u%%，大约 %u 分钟、省 %s 元。",
                 km_text, caveat, now_text, when, cheap_text,
                 (unsigned)target, (unsigned)out->charge_minutes, save_text);
    } else {
        out->action = CHARGE_PLAN_CHARGE_NOW;
        format_km(km_text, sizeof(km_text), s_config.daily_km);
        snprintf(out->reason, sizeof(out->reason),
                 "明天大约骑 %s 公里%s，现在电价合适，可以充到 %u%%，大约需要 %u 分钟。",
                 km_text, caveat, (unsigned)target, (unsigned)out->charge_minutes);
    }

    ESP_LOGI(TAG, "充电建议：%s soc=%u%% target=%u%% energy=%.2fkWh minutes=%u temp=%d factor=%.2f",
             charge_strategy_action_name(out->action), (unsigned)soc, (unsigned)target,
             (double)out->energy_kwh, (unsigned)out->charge_minutes, (int)hottest,
             (double)factor);
    return true;
}
