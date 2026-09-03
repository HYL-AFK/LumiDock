#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Timer slots owned by this service: one one-shot alarm and two countdowns.
 * The stopwatch is a single in-memory stopwatch and never persists. */
#define TIMER_SERVICE_MAX_ALARMS 1U
#define TIMER_SERVICE_MAX_COUNTDOWNS 2U
#define TIMER_SERVICE_MAX_DISPLAY_SECONDS (59U * 60U + 59U)
/* While a timer rings, the event callback receives one RING heartbeat every
 * 2 s so the caller can replay the chime; after 30 s it auto-stops. */
#define TIMER_SERVICE_RING_HEARTBEAT_MS 2000U
#define TIMER_SERVICE_RING_TIMEOUT_MS 30000U
/* Countdown progress is flushed to NVS every N ticks to limit flash wear;
 * a power cut can lose at most this many seconds of progress. */
#define TIMER_SERVICE_COUNTDOWN_PERSIST_TICKS 30U

typedef enum {
    TIMER_SERVICE_SLOT_ALARM0 = 0,
    TIMER_SERVICE_SLOT_COUNTDOWN0,
    TIMER_SERVICE_SLOT_COUNTDOWN1,
    TIMER_SERVICE_SLOT_COUNT,
} timer_service_slot_t;

typedef enum {
    TIMER_SERVICE_EVENT_RING = 0, /* heartbeat while a timer is ringing */
    TIMER_SERVICE_EVENT_STOPPED,  /* ringing finished (auto, key, or voice) */
} timer_service_event_t;

typedef enum {
    TIMER_SERVICE_CMD_NOT_HANDLED = 0,
    TIMER_SERVICE_CMD_EXECUTED,
    TIMER_SERVICE_CMD_FAILED,
} timer_service_cmd_result_t;

typedef struct {
    bool alarm_active;
    uint8_t alarm_hour;
    uint8_t alarm_minute;
    bool countdown_active;
    uint32_t countdown_remaining_seconds;
    bool stopwatch_running;
    uint32_t stopwatch_seconds;
    bool ringing;
} timer_service_snapshot_t;

typedef void (*timer_service_event_cb_t)(timer_service_event_t event,
                                         timer_service_slot_t slot);

esp_err_t timer_service_init(timer_service_event_cb_t event_cb);
/* Call every second (from the snapshot bridge or an internal tick task). */
void timer_service_poll(void);

/* One-shot alarm: rings at the next occurrence of hour:minute (tomorrow if
 * the time already passed).  Requires a synchronized clock. */
timer_service_cmd_result_t timer_service_set_alarm(uint8_t hour, uint8_t minute);
/* Cancels every pending alarm matching hour:minute (regardless of whether
 * it targets today or tomorrow). */
timer_service_cmd_result_t timer_service_cancel_alarm(uint8_t hour, uint8_t minute);
/* Countdown: rings after duration_seconds.  No clock required. */
timer_service_cmd_result_t timer_service_set_countdown(uint32_t duration_seconds);
/* Voice commands reserve a countdown until their acknowledgement TTS has
 * either drained or the voice chain has failed and closed. */
timer_service_cmd_result_t timer_service_prepare_countdown(uint32_t duration_seconds);
timer_service_cmd_result_t timer_service_start_pending_countdowns(void);
void timer_service_cancel_pending_countdowns(void);
/* Stopwatch: start/pause/resume/stop, in-memory only. */
timer_service_cmd_result_t timer_service_start_stopwatch(void);
timer_service_cmd_result_t timer_service_pause_stopwatch(void);
timer_service_cmd_result_t timer_service_stop_stopwatch(void);
bool timer_service_query_stopwatch(uint32_t *seconds_out);
/* Smallest positive remaining countdown, for "还剩多久" queries. */
bool timer_service_query_countdown_remaining(uint32_t *seconds_out);
void timer_service_get_snapshot(timer_service_snapshot_t *out);

bool timer_service_is_ringing(void);
/* Stops the current ringing (key press, voice command, or wake-word snooze).
 * Idempotent; also clears queued ring requests for the stopped slot. */
void timer_service_stop_ringing(void);

/* True when the SNTP clock has converged (alarms need this). */
bool timer_service_is_time_synced(void);
