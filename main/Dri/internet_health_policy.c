#include "internet_health_policy.h"

#include <stddef.h>

#define INTERNET_HEALTH_FAILURE_LIMIT 3U

void internet_health_policy_init(internet_health_policy_t *policy)
{
    if (policy == NULL) return;
    policy->online = false;
    policy->consecutive_failures = 0;
}

internet_health_action_t internet_health_policy_record(internet_health_policy_t *policy,
                                                       bool probe_ok)
{
    if (policy == NULL) return INTERNET_HEALTH_NO_ACTION;

    if (probe_ok) {
        policy->online = true;
        policy->consecutive_failures = 0;
        return INTERNET_HEALTH_ONLINE;
    }

    policy->online = false;
    if (policy->consecutive_failures < INTERNET_HEALTH_FAILURE_LIMIT) {
        policy->consecutive_failures++;
    }
    if (policy->consecutive_failures >= INTERNET_HEALTH_FAILURE_LIMIT) {
        policy->consecutive_failures = 0;
        return INTERNET_HEALTH_RECONNECT;
    }
    return INTERNET_HEALTH_NO_ACTION;
}
