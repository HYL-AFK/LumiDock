#include "button_driver.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "watchdog_service.h"

#define BUTTON_SAMPLE_MS 20
#define BUTTON_LONG_PRESS_MS 2000

static button_long_press_cb_t s_long_press_cb;
static button_short_press_cb_t s_short_press_cb;
static TaskHandle_t s_task;

static void button_task(void *arg)
{
    (void)arg;
    (void)watchdog_service_subscribe_current_task();
    bool pressed = false;
    bool notified = false;
    int64_t pressed_at_us = 0;
    for (;;) {
        watchdog_service_feed();
        const bool is_pressed = gpio_get_level(LUMIDOCK_BUTTON_GPIO) == 0;
        if (is_pressed && !pressed) {
            pressed = true;
            notified = false;
            pressed_at_us = esp_timer_get_time();
        } else if (!is_pressed) {
            if (pressed && !notified && s_short_press_cb != NULL) s_short_press_cb();
            pressed = false;
            notified = false;
        } else if (pressed && !notified &&
                   esp_timer_get_time() - pressed_at_us >=
                       (int64_t)BUTTON_LONG_PRESS_MS * 1000) {
            notified = true;
            if (s_long_press_cb != NULL) s_long_press_cb();
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_SAMPLE_MS));
    }
}

esp_err_t button_driver_init(void)
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
    if (s_task == NULL &&
        xTaskCreate(button_task, "button_service", 2048, NULL, 2, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t button_driver_set_long_press_callback(button_long_press_cb_t callback)
{
    s_long_press_cb = callback;
    return ESP_OK;
}

esp_err_t button_driver_set_short_press_callback(button_short_press_cb_t callback)
{
    s_short_press_cb = callback;
    return ESP_OK;
}
