#include "internet_health_policy.h"

#include <stddef.h>

/* 软失败（有回包但内容不合规）要连中三次——链路是通的，重连本来也帮不上什么忙，
 * 多观察几轮免得被对端的偶发异常带着来回重连。
 *
 * 硬失败（压根连不上）两次就收场。一次可能只是信道瞬时抖动，但相隔两秒还是建不起
 * 连接，就已经不是抖动而是链路假死了。实测那次假死持续了 33 秒：电台先饿死，
 * 用户唤醒时只能冷启动去撞最坏的网络，DNS 直接 EAI_FAIL，全程只响一声错误音。
 * 原来的三次门槛在最坏情况下要 35 秒以上才判定，比故障本身还长。 */
#define INTERNET_HEALTH_FAILURE_LIMIT 3U
#define INTERNET_HEALTH_HARD_FAILURE_LIMIT 2U

void internet_health_policy_init(internet_health_policy_t *policy)
{
    if (policy == NULL) return;
    policy->online = false;
    policy->consecutive_failures = 0;
    policy->consecutive_hard_failures = 0;
}

internet_health_action_t internet_health_policy_record(internet_health_policy_t *policy,
                                                       internet_probe_outcome_t outcome)
{
    if (policy == NULL) return INTERNET_HEALTH_NO_ACTION;

    if (outcome == INTERNET_PROBE_OUTCOME_OK) {
        policy->online = true;
        policy->consecutive_failures = 0;
        policy->consecutive_hard_failures = 0;
        return INTERNET_HEALTH_ONLINE;
    }

    policy->online = false;
    if (policy->consecutive_failures < INTERNET_HEALTH_FAILURE_LIMIT) {
        policy->consecutive_failures++;
    }
    if (outcome == INTERNET_PROBE_OUTCOME_HARD_FAILURE &&
        policy->consecutive_hard_failures < INTERNET_HEALTH_HARD_FAILURE_LIMIT) {
        policy->consecutive_hard_failures++;
    }
    if (policy->consecutive_failures >= INTERNET_HEALTH_FAILURE_LIMIT ||
        policy->consecutive_hard_failures >= INTERNET_HEALTH_HARD_FAILURE_LIMIT) {
        policy->consecutive_failures = 0;
        policy->consecutive_hard_failures = 0;
        return INTERNET_HEALTH_RECONNECT;
    }
    return INTERNET_HEALTH_NO_ACTION;
}
