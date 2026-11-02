#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Initializes the supervisor and configures the ESP-IDF task watchdog. */
esp_err_t watchdog_service_init(void);

/* Registers the calling application task for software heartbeat monitoring. */
esp_err_t watchdog_service_subscribe_current_task(void);

/* Records a heartbeat for the calling application task. */
void watchdog_service_feed(void);

/* Opts the calling task into reboot-on-stale-heartbeat.  Every task is
 * report-only by default, because a late heartbeat on a saturated core is a
 * scheduling symptom rather than a hang and must not restart the device. */
void watchdog_service_set_current_task_fatal(bool fatal);

/* Marks a bounded blocking network operation for the calling task. */
void watchdog_service_set_current_task_blocking(bool blocking);

/* Temporarily suppresses stale-heartbeat reporting during controlled startup/shutdown. */
void watchdog_service_set_business_suspended(bool suspended);

/* Asks the supervisor to stop feeding the hardware watchdog so the device
 * resets itself. Used by the fault registry when a level-1 fault (core
 * capability lost) has stayed mounted past its grace window.
 *
 * Deliberately routed through the same latch as a stale heartbeat instead of
 * calling esp_restart() directly: the supervisor stops feeding only after it
 * has taken the slot lock, so the panic path still runs with the rest of the
 * system intact and the reason is visible in the log that precedes the reset.
 * Returns ESP_ERR_INVALID_STATE before init. */
esp_err_t watchdog_service_request_reset(const char *reason);
