#include "onboard_led_driver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

#include "board_pins.h"

/*
 * On-board single WS2812B, bit-banged directly on GPIO48. No RMT/SPI used.
 * The CPU runs at 240 MHz (1 cycle = 4.1667 ns). WS2812B timing:
 *   logical 0: T0H ~0.25 us high + T0L ~1.00 us low
 *   logical 1: T1H ~0.72 us high + T1L ~0.53 us low
 * A frame is 3 bytes (green, red, blue), MSB first, then a long low reset
 * pulse (>50 us) to latch. Same GRB ordering as ws2812_driver.
 *
 * Register-level writes (gpio_ll_set_level) replace gpio_set_level() so the
 * per-bit function-call overhead no longer stretches the logic-0 high pulse
 * over the ~0.4 us 0/1 decision threshold (which made every bit read as 1
 * => persistent white). The cycle counts land in the safe middle of each
 * timing band, so even +~5 cycles of loop overhead cannot cross a threshold.
 */
#define T0H_CYCLES 60U   /* ~0.25 us high  (logic-0 pulse, safely < threshold) */
#define T0L_CYCLES 240U  /* ~1.00 us low             */
#define T1H_CYCLES 172U  /* ~0.72 us high (logic-1 pulse, safely > threshold) */
#define T1L_CYCLES 128U  /* ~0.53 us low             */
#define RESET_CYCLES 30000U /* ~125 us reset/latch */

static const char *TAG = "onboard_led";

/* Global brightness cap for the single on-board LED. 10% keeps the marker
 * non-glaring while preserving each state's hue (session color untouched).
 * Applies uniformly to boot-blue / fault-red / steady states on GPIO48. */
#define ONBOARD_LED_BRIGHTNESS_PERCENT 10U

static bool s_initialized;
static uint8_t s_grb[3];

static uint8_t brightness_scale(uint8_t channel)
{
    return (uint8_t)(((uint16_t)channel * ONBOARD_LED_BRIGHTNESS_PERCENT) / 100U);
}

static inline void delay_cycles(uint32_t cycles)
{
    const uint32_t start = esp_cpu_get_cycle_count();
    while ((uint32_t)(esp_cpu_get_cycle_count() - start) < cycles) {
    }
}

static inline void gpio_high(void)
{
    gpio_ll_set_level(&GPIO, LUMIDOCK_ONBOARD_LED_GPIO, 1);
}

static inline void gpio_low(void)
{
    gpio_ll_set_level(&GPIO, LUMIDOCK_ONBOARD_LED_GPIO, 0);
}

static inline void output_bit(bool one)
{
    gpio_high();
    delay_cycles(one ? T1H_CYCLES : T0H_CYCLES);
    gpio_low();
    delay_cycles(one ? T1L_CYCLES : T0L_CYCLES);
}

static esp_err_t send_frame(void)
{
    /* Disable interrupts on this core so the ~125 us frame is never broken. */
    portDISABLE_INTERRUPTS();
    for (size_t byte = 0U; byte < sizeof(s_grb); ++byte) {
        const uint8_t value = s_grb[byte];
        for (int8_t bit = 7; bit >= 0; --bit) {
            output_bit(((value >> bit) & 0x01U) != 0U);
        }
    }
    gpio_low();
    delay_cycles(RESET_CYCLES);
    portENABLE_INTERRUPTS();
    return ESP_OK;
}

esp_err_t onboard_led_driver_init(void)
{
    if (s_initialized) return ESP_OK;

    const gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << LUMIDOCK_ONBOARD_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t err = gpio_config(&io_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_low();

    s_initialized = true;
    ESP_LOGI(TAG, "on-board WS2812B bit-bang on GPIO%d (240 MHz)",
             LUMIDOCK_ONBOARD_LED_GPIO);
    return ESP_OK;
}

esp_err_t onboard_led_driver_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    s_grb[0] = brightness_scale(green);
    s_grb[1] = brightness_scale(red);
    s_grb[2] = brightness_scale(blue);
    return send_frame();
}

void onboard_led_driver_clear(void)
{
    if (!s_initialized) return;
    s_grb[0] = 0U;
    s_grb[1] = 0U;
    s_grb[2] = 0U;
    (void)send_frame();
}
