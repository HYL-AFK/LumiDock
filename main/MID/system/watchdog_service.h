#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Initializes the supervisor and configures the ESP-IDF task watchdog. */
esp_err_t watchdog_service_init(void);

/* Registers the calling application task for software heartbeat monitoring. */
esp_err_t watchdog_service_subscribe_current_task(void);

/* Records a heartbeat for the calling application task. */
void watchdog_service_feed(void);

/* Marks a bounded blocking network operation for the calling task. */
void watchdog_service_set_current_task_blocking(bool blocking);

/* Temporarily suppresses stale-heartbeat reporting during controlled startup/shutdown. */
void watchdog_service_set_business_suspended(bool suspended);
