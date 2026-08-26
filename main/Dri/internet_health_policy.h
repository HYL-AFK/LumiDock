#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    INTERNET_HEALTH_NO_ACTION = 0,
    INTERNET_HEALTH_ONLINE,
    INTERNET_HEALTH_RECONNECT,
} internet_health_action_t;

typedef struct {
    bool online;
    uint8_t consecutive_failures;
} internet_health_policy_t;

void internet_health_policy_init(internet_health_policy_t *policy);
internet_health_action_t internet_health_policy_record(internet_health_policy_t *policy,
                                                       bool probe_ok);
