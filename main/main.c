#include "app_manager.h"
#include "startup_gate.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LUMIDOCK_NETWORK_TEST_MODE 1

static const char *TAG = "main";

void app_main(void)
{
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
