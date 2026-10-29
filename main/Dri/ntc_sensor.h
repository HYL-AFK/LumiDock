#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* NTC thermistor on the dedicated ADC1 channel.
 *
 * Divider: VCC -- NTC -- node(ADC) -- 10k -- GND. The node voltage therefore
 * rises with temperature, which keeps the strapping pin (GPIO3) high at boot
 * in every realistic operating range. */

esp_err_t ntc_sensor_init(void);

bool ntc_sensor_is_ready(void);

/* Latest temperature in degrees Celsius.
 *
 * Consecutive conversions are averaged inside the driver so a single noisy
 * sample cannot move the fan curve. Returns ESP_ERR_INVALID_RESPONSE when the
 * divider sits at a rail, which means the probe is open or shorted rather
 * than reading an impossibly cold or hot value. */
esp_err_t ntc_sensor_read_celsius(float *out_celsius);

/* Same conversion, but also hands back the divider node voltage.
 *
 * Calibration wants the number a multimeter would read at the ADC pin: if the
 * voltage agrees but the temperature does not, the Beta fit is wrong; if the
 * voltage is off, it is the ADC path. Either pointer may be NULL. */
esp_err_t ntc_sensor_read_sample(int *out_millivolts, float *out_celsius);
