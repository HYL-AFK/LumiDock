#include "startup_coordinator.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "i2s_audio_driver.h"
#include "status_led_service.h"
#include "startup_audio.h"
#include "ui_service.h"
#include "xiaozhi_activation_service.h"
#include "xiaozhi_voice_service.h"

#define STARTUP_COORDINATOR_POLL_MS 50U
#define STARTUP_SOUND_REMAINING_US (4000000LL)

static const char *TAG = "startup_coord";
static bool s_initialized;

static void startup_coordinator_task(void *arg)
{
    (void)arg;
    bool sound_attempted = false;
    bool boot_led_released = false;
    bool greeting_requested = false;

    status_led_service_set_boot_animation_active(true);
    ESP_LOGI(TAG, "boot coordinator started; green trail follows UI boot animation");

    for (;;) {
        if (!sound_attempted && ui_service_boot_sound_window_open()) {
            sound_attempted = true;
            ESP_LOGI(TAG, "boot sound trigger: final %lld ms of minimum boot window",
                     (long long)(STARTUP_SOUND_REMAINING_US / 1000LL));
            if (!i2s_audio_driver_is_ready()) {
                ESP_LOGW(TAG, "startup PCM skipped: ES8311 is not ready");
            } else {
                (void)startup_audio_play_ready();
            }
            ui_service_note_boot_sound_finished();
        }

        if (!boot_led_released && !ui_service_boot_is_active()) {
            boot_led_released = true;
            status_led_service_set_boot_animation_active(false);
            ESP_LOGI(TAG, "boot animation ended; status LED returned to normal ownership");
        }

        if (!greeting_requested && ui_service_boot_transition_complete() &&
            ui_service_boot_cloud_ready() &&
            xiaozhi_activation_service_is_activated() && xiaozhi_voice_service_is_ready()) {
            const esp_err_t err = xiaozhi_voice_service_start_boot_greeting();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "boot greeting request rejected: %s", esp_err_to_name(err));
            } else {
                greeting_requested = true;
                ESP_LOGI(TAG, "boot greeting requested after network decision and tested voice pipeline became ready");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(STARTUP_COORDINATOR_POLL_MS));
    }
}

esp_err_t startup_coordinator_init(void)
{
    if (s_initialized) return ESP_OK;
    if (xTaskCreatePinnedToCoreWithCaps(startup_coordinator_task, "startup_coord", 4096,
                                        NULL, 2, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    return ESP_OK;
}
