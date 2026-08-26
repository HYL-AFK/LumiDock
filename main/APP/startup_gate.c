#include "startup_gate.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"

#define STARTUP_GATE_HOLD_MS 3000
#define STARTUP_GATE_SAMPLE_MS 20

static const char *TAG = "startup_gate";

static esp_err_t configure_button_wakeup(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << LUMIDOCK_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;

    /* The confirmed button GPIO is RTC-capable; keep its pull-up active in sleep. */
    err = rtc_gpio_init(LUMIDOCK_BUTTON_GPIO);
    if (err != ESP_OK) return err;
    err = rtc_gpio_set_direction(LUMIDOCK_BUTTON_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    if (err != ESP_OK) return err;
    err = rtc_gpio_pullup_en(LUMIDOCK_BUTTON_GPIO);
    if (err != ESP_OK) return err;
    err = rtc_gpio_pulldown_dis(LUMIDOCK_BUTTON_GPIO);
    if (err != ESP_OK) return err;

    return esp_sleep_enable_ext0_wakeup(LUMIDOCK_BUTTON_GPIO, 0);
}

esp_err_t startup_gate_init(void)
{
    return configure_button_wakeup();
}

static bool button_held_for_start(void)
{
    if (gpio_get_level(LUMIDOCK_BUTTON_GPIO) != 0) return false;

    const int64_t started_us = esp_timer_get_time();
    while (gpio_get_level(LUMIDOCK_BUTTON_GPIO) == 0) {
        if (esp_timer_get_time() - started_us >= (int64_t)STARTUP_GATE_HOLD_MS * 1000) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(STARTUP_GATE_SAMPLE_MS));
    }
    return false;
}

bool startup_gate_wait_for_long_press(void)
{
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const bool woke_from_button = cause == ESP_SLEEP_WAKEUP_EXT0;
    if (woke_from_button || gpio_get_level(LUMIDOCK_BUTTON_GPIO) == 0) {
        if (button_held_for_start()) {
            (void)rtc_gpio_deinit(LUMIDOCK_BUTTON_GPIO);
            ESP_LOGI(TAG, "button held for %d ms; starting application", STARTUP_GATE_HOLD_MS);
            return true;
        }
        ESP_LOGI(TAG, "button released before startup hold; returning to deep sleep");
    } else {
        ESP_LOGI(TAG, "power-on without startup hold; entering deep sleep");
    }

    startup_gate_enter_deep_sleep();
    return false;
}

void startup_gate_enter_deep_sleep(void)
{
    if (configure_button_wakeup() != ESP_OK) {
        ESP_LOGE(TAG, "failed to restore button RTC wake configuration");
    }
    ESP_LOGI(TAG, "entering deep sleep; hold the button for %d ms to wake", STARTUP_GATE_HOLD_MS);
    esp_deep_sleep_start();
}
