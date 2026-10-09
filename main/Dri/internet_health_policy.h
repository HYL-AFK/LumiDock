#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    INTERNET_HEALTH_NO_ACTION = 0,
    INTERNET_HEALTH_ONLINE,
    INTERNET_HEALTH_RECONNECT,
} internet_health_action_t;

/* 探测结果按“能不能把一条新的出网连接建起来”分两级。
 *
 * 这个区分直接来自实测：电台播放中链路会进入“已关联、IP 没变、但新连接一律建不起来”
 * 的假死状态——彼时 esp-tls 报 select() timeout、getaddrinfo returns 202，而已经建好的
 * 那条流还在慢慢走。软失败（拿到回包但内容不合规）说明链路是通的，重连帮不上忙，多观察
 * 几次再动；硬失败（压根连不上）才是必须尽快收场的那一种。 */
typedef enum {
    INTERNET_PROBE_OUTCOME_OK = 0,
    INTERNET_PROBE_OUTCOME_SOFT_FAILURE,
    INTERNET_PROBE_OUTCOME_HARD_FAILURE,
} internet_probe_outcome_t;

typedef struct {
    bool online;
    /* 连续失败次数，软硬都算。 */
    uint8_t consecutive_failures;
    /* 其中“连不上”的连续次数。硬失败单独计数是为了让它更快到线：
       软失败要连中三次，硬失败两次就够。 */
    uint8_t consecutive_hard_failures;
} internet_health_policy_t;

void internet_health_policy_init(internet_health_policy_t *policy);
internet_health_action_t internet_health_policy_record(internet_health_policy_t *policy,
                                                       internet_probe_outcome_t outcome);
