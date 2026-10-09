#include <assert.h>
#include <stdbool.h>

#include "internet_health_policy.h"

int main(void)
{
    internet_health_policy_t policy;

    /* 软失败：有回包但内容不合规。链路是通的，重连帮不上什么忙，所以仍然要求
       连中三次——不能被对端的偶发异常带着来回重连。 */
    internet_health_policy_init(&policy);
    assert(!policy.online);
    assert(policy.consecutive_failures == 0);
    assert(policy.consecutive_hard_failures == 0);
    assert(internet_health_policy_record(&policy, INTERNET_PROBE_OUTCOME_SOFT_FAILURE) ==
           INTERNET_HEALTH_NO_ACTION);
    assert(policy.consecutive_failures == 1);
    assert(policy.consecutive_hard_failures == 0);
    assert(internet_health_policy_record(&policy, INTERNET_PROBE_OUTCOME_SOFT_FAILURE) ==
           INTERNET_HEALTH_NO_ACTION);
    assert(policy.consecutive_failures == 2);
    assert(policy.consecutive_hard_failures == 0);
    assert(internet_health_policy_record(&policy, INTERNET_PROBE_OUTCOME_SOFT_FAILURE) ==
           INTERNET_HEALTH_RECONNECT);
    assert(!policy.online);
    assert(policy.consecutive_failures == 0);
    assert(policy.consecutive_hard_failures == 0);

    /* 硬失败：压根连不上。实测里电台播放中链路假死就是这个形态——关联还在、
       IP 没变，但新连接一律建不起来（select() timeout / getaddrinfo 202），
       而已经建好的那条流还在慢慢走。两次就到线：原来的三次门槛在最坏情况下
       要 35 秒以上，比故障本身还长。 */
    internet_health_policy_init(&policy);
    assert(internet_health_policy_record(&policy, INTERNET_PROBE_OUTCOME_HARD_FAILURE) ==
           INTERNET_HEALTH_NO_ACTION);
    assert(policy.consecutive_hard_failures == 1);
    assert(policy.consecutive_failures == 1);
    assert(internet_health_policy_record(&policy, INTERNET_PROBE_OUTCOME_HARD_FAILURE) ==
           INTERNET_HEALTH_RECONNECT);
    assert(policy.consecutive_hard_failures == 0);
    assert(policy.consecutive_failures == 0);

    /* 单次硬失败绝不能触发重连：信道瞬时抖动也会让一次连接超时，为它重挂一次
       WiFi 得不偿失。而且信号一旦回来，两个计数都必须清零，否则下一次抖动会
       叠加到线上，把偶发故障升级成真实的断网。 */
    internet_health_policy_init(&policy);
    assert(internet_health_policy_record(&policy, INTERNET_PROBE_OUTCOME_HARD_FAILURE) ==
           INTERNET_HEALTH_NO_ACTION);
    assert(internet_health_policy_record(&policy, INTERNET_PROBE_OUTCOME_OK) ==
           INTERNET_HEALTH_ONLINE);
    assert(policy.online);
    assert(policy.consecutive_failures == 0);
    assert(policy.consecutive_hard_failures == 0);
    assert(internet_health_policy_record(&policy, INTERNET_PROBE_OUTCOME_HARD_FAILURE) ==
           INTERNET_HEALTH_NO_ACTION);

    /* 软硬混着来：硬失败只有一次，靠总次数第三次到线；触发后两个计数一起清零，
       不能让上一轮的硬失败残留下来污染下一轮判定。 */
    internet_health_policy_init(&policy);
    assert(internet_health_policy_record(&policy, INTERNET_PROBE_OUTCOME_HARD_FAILURE) ==
           INTERNET_HEALTH_NO_ACTION);
    assert(internet_health_policy_record(&policy, INTERNET_PROBE_OUTCOME_SOFT_FAILURE) ==
           INTERNET_HEALTH_NO_ACTION);
    assert(internet_health_policy_record(&policy, INTERNET_PROBE_OUTCOME_SOFT_FAILURE) ==
           INTERNET_HEALTH_RECONNECT);
    assert(policy.consecutive_failures == 0);
    assert(policy.consecutive_hard_failures == 0);

    return 0;
}
