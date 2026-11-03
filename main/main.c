#include "app_manager.h"
#include "board_pins.h"
#include "startup_gate.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LUMIDOCK_NETWORK_TEST_MODE 1

static const char *TAG = "main";

/* 功放 NS4150B 的使能脚没引到板上，它从通电那一刻就开着，而它前面挂的 GPIO7
 *（ES8311 的 DIN）在上电后一直是高阻。输入端悬空是模拟电路里最典型的噪声源：
 * 自激、拾环境噪声都在这个时候发生，听感就是开机头一两秒的“滋啦”。
 *
 * 这里在 app_main 的第一行就给功放输入一个确定的低电平，把它拉开悬空的区间；
 * 等到 I2S 初始化时这根脚会被重新配成 DOUT 接管过去，不需要再收回。 */
static void quiet_amp_input_until_i2s_takes_over(void)
{
    const gpio_config_t amp_din = {
        .pin_bit_mask = 1ULL << LUMIDOCK_I2S_AMP_DIN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&amp_din) == ESP_OK) {
        (void)gpio_set_level(LUMIDOCK_I2S_AMP_DIN_GPIO, 0);
    }
}

void app_main(void)
{
    quiet_amp_input_until_i2s_takes_over();

#if LUMIDOCK_NETWORK_TEST_MODE
    ESP_LOGW(TAG, "network test mode: button startup gate bypassed");
    esp_err_t err = app_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "startup failed: %s", esp_err_to_name(err));
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
#else
    esp_err_t err = startup_gate_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "startup gate init failed: %s", esp_err_to_name(err));
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (!startup_gate_wait_for_long_press()) {
        return;
    }

    err = app_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "startup failed: %s", esp_err_to_name(err));
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
#endif
}
