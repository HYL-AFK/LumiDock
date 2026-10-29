#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Temperature-driven fan policy.
 *
 * Owns the thermistor-to-duty translation so neither the driver nor the UI has
 * to know about tiers or hysteresis. The mapping is a step function, not a
 * curve: below 40 C the fan is off, 40-60 C runs 50%, 60-90 C runs 70% and
 * above 90 C it runs flat out. Each tier is left only once the temperature
 * falls THERMAL_HYSTERESIS_C below the threshold that let it in, so a reading
 * hovering on a boundary cannot make the rotor cycle. */

esp_err_t thermal_service_init(void);

/* Called once per second from the snapshot bridge: samples the thermistor,
 * converts it into a fan duty and applies it. */
void thermal_service_poll(void);

/* Last accepted temperature in Celsius, or NAN before the first good sample.
 * A probe that is missing or shorted keeps this at NAN rather than reporting
 * a fabricated value. */
float thermal_service_get_temperature(void);

/* Duty actually applied to the fan, 0..100. */
uint8_t thermal_service_get_fan_percent(void);

/* Measured rotor speed; 0 when the fan is stopped or has no tachometer. */
uint32_t thermal_service_get_fan_rpm(void);
