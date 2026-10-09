#ifndef CHARGE_STRATEGY_H
#define CHARGE_STRATEGY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    CHARGE_PLAN_BLOCKED = 0, /* 安全原因不建议充（低温/高温） */
    CHARGE_PLAN_NO_NEED,     /* 电量够明天用，不用充 */
    CHARGE_PLAN_CHARGE_NOW,  /* 现在就可以充 */
    CHARGE_PLAN_DEFER,       /* 建议等到更便宜的时段再充 */
} charge_plan_action_t;

/* 按各字段上界估算，DEFER 分支最长约 361 字节。取 512 而不是 384，是给 GCC 对
 * %s 的上界估算留余量——它随上下文浮动，卡着边界写太容易再挂一次构建。
 * 这个结构体只在 PSRAM 里有一份，多占一百多字节换构建稳定，值。 */
#define CHARGE_REASON_MAX 512U
#define CHARGE_WARNING_MAX 128U

typedef struct {
    charge_plan_action_t action;
    bool should_charge;
    /* 给云端播报用的整句话，云端直接念，不要自己再组织。 */
    char reason[CHARGE_REASON_MAX];
    /* 安全提醒（低温禁充之类），没有则是空串。 */
    char warning[CHARGE_WARNING_MAX];
    int64_t start_ts; /* 建议开始充电（unix 秒）；NOW 时是 0 */
    int64_t end_ts;   /* 预计结束 */
    uint8_t target_soc;
    uint8_t current_soc;
    int16_t battery_temp_c; /* 最热那一路 */
    float energy_kwh;       /* 需要补进去的电量 */
    float cost_est;         /* 预计花费（元） */
    float saving_est;       /* 等到低价时段能省多少（元） */
    uint16_t charge_minutes;
} charge_plan_t;

typedef struct {
    /* 设备没有里程计，这两个是固定配置值：按自己的车型填一次即可。
     * wh_per_km = 电池总能量(Wh) ÷ 实际满电续航(km)。 */
    float daily_km;
    float wh_per_km;
    /* 充电器功率，用来估时长。 */
    float charge_power_w;
    uint8_t target_soc;       /* 日常充到多少 */
    uint8_t min_soc;          /* 低于它就优先充，不为省电费拖延 */
    uint8_t departure_hour;   /* 出发时间，用来判断低价窗口来不来得及 */
    uint8_t departure_minute;
} charge_config_t;

void charge_strategy_init(void);

void charge_strategy_get_config(charge_config_t *out);
/* 只覆盖传进来的字段所在的结构体，不做范围纠正之外的加工。 */
void charge_strategy_set_config(const charge_config_t *cfg);

/* 生成一份充电建议。电池数据不可用时返回 false（此时不该硬给建议）。 */
bool charge_strategy_build_plan(charge_plan_t *out);

const char *charge_strategy_action_name(charge_plan_action_t action);

#endif
