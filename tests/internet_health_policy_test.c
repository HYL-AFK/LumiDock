#include <assert.h>
#include <stdbool.h>

#include "internet_health_policy.h"

int main(void)
{
    internet_health_policy_t policy;
    internet_health_policy_init(&policy);

    assert(!policy.online);
    assert(policy.consecutive_failures == 0);
    assert(internet_health_policy_record(&policy, false) == INTERNET_HEALTH_NO_ACTION);
    assert(policy.consecutive_failures == 1);
    assert(internet_health_policy_record(&policy, false) == INTERNET_HEALTH_NO_ACTION);
    assert(policy.consecutive_failures == 2);
    assert(internet_health_policy_record(&policy, false) == INTERNET_HEALTH_RECONNECT);
    assert(!policy.online);
    assert(policy.consecutive_failures == 0);

    assert(internet_health_policy_record(&policy, true) == INTERNET_HEALTH_ONLINE);
    assert(policy.online);
    assert(policy.consecutive_failures == 0);
    assert(internet_health_policy_record(&policy, false) == INTERNET_HEALTH_NO_ACTION);
    assert(internet_health_policy_record(&policy, true) == INTERNET_HEALTH_ONLINE);
    assert(policy.online);
    assert(policy.consecutive_failures == 0);
    return 0;
}
