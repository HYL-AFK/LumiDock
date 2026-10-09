#ifndef TARIFF_SERVICE_H
#define TARIFF_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* awattar 每小时给一条，取 24 条正好覆盖一整天。 */
#define TARIFF_MAX_SLOTS 24U

typedef struct {
    int64_t start_ts; /* Unix 秒（UTC） */
    float price_ct;   /* ct/kWh，已含附加费 */
} tariff_slot_t;

typedef struct {
    bool valid;
    bool from_network; /* true=接口拉的；false=本地峰谷兜底 */
    int64_t updated_at_ts;
    /* 接口给的是批发电价（EPEX Spot），不是到户价。这个标记必须留着：
     * 忘了加 surcharge_ct 会让"能省多少钱"的估算差出二十多 ct/kWh。 */
    bool is_wholesale;
    float surcharge_ct;
    uint8_t slot_count;
    tariff_slot_t slots[TARIFF_MAX_SLOTS];
    /* 派生值，省得每个调用方各算一遍。 */
    float current_ct;
    float avg_ct;
    float min_ct;
    float max_ct;
    int64_t cheapest_start_ts; /* 当日最低价那一小时的起点 */
} tariff_snapshot_t;

esp_err_t tariff_service_init(void);

/* 网络就绪时触发一次立即刷新，不用等下一个周期。 */
void tariff_service_notify_network(void);

bool tariff_service_get(tariff_snapshot_t *out);

/* 本轮用不上的电价服务也要能拿到一份可用数据：接口失败时用本地峰谷合成
 * 24 小时槽位。充电配置服务读回 NVS 后调用它覆盖这里的默认值。 */
void tariff_service_set_fallback(uint8_t offpeak_start_hour, uint8_t offpeak_end_hour,
                                 float offpeak_ct, float peak_ct);

/* 在快照里找连续 hours 小时均价最低的窗口。数据不够或参数非法返回 false。 */
bool tariff_service_find_cheapest(const tariff_snapshot_t *snap, uint8_t hours,
                                  int64_t *start_ts, float *avg_ct);

#endif
