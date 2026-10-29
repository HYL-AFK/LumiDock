#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 4-wire fan driver.
 *
 * The PWM pin takes the Intel 25 kHz control input, so the fan's own driver
 * stage keeps VCC constant - no chopping on the supply rail, which matters
 * because this board shares it with the BMS and the audio codec.
 *
 * The FG pin is an open collector tachometer that pulses twice per
 * revolution; it is counted in hardware so a busy scheduler cannot lose
 * pulses. */

esp_err_t fan_control_init(void);

bool fan_control_is_ready(void);

/* Requested speed, 0..100. Zero stops the rotor; any other value is clamped up
 * to the minimum duty a 4-wire fan needs to keep turning, so asking for 5%
 * never leaves the fan stalled or hunting. */
esp_err_t fan_control_set_percent(uint8_t percent);

uint8_t fan_control_get_percent(void);

/* Rotations per minute measured over the interval since the previous call.
 * Returns 0 while stopped or while the sample window is still too short. */
uint32_t fan_control_get_rpm(void);
