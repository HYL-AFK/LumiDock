#include "app_manager.h"

#include <math.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bms_service.h"
#include "ble_provision_service.h"
#include "button_driver.h"
#include "i2s_audio_driver.h"
#include "mid_snapshot.h"
#include "runtime_event_log.h"
#include "status_led_service.h"
#include "stc_service.h"
#include "startup_coordinator.h"
#include "time_service.h"
#include "timer_service.h"
#include "weather_service.h"
#include "wifi_service.h"
#include "xiaozhi_activation_service.h"
#include "xiaozhi_voice_service.h"
#include "ui_service.h"
#include "watchdog_service.h"
#include "radio_stream_service.h"

static const char *TAG = "app_manager";
static SemaphoreHandle_t i2s_audio_driver_init_lock;

/* Temporary bench profile: exercise only network provisioning, cloud data and display. */
#define LUMIDOCK_NETWORK_UI_TEST_MODE 1
#define LUMIDOCK_VOICE_TEST_MODE 1
#define LUMIDOCK_AUDIO_TEST_ENABLED (LUMIDOCK_VOICE_TEST_MODE || !LUMIDOCK_NETWORK_UI_TEST_MODE)
#define AUDIO_RETRY_DELAY_MS 3000U
#define AUDIO_SELF_TEST_SAMPLES LUMIDOCK_AUDIO_SAMPLE_RATE_HZ
#define VOICE_INITIAL_NETWORK_SETTLE_TIMEOUT_MS 15000U
#define VOICE_WSS_TASK_STACK_BYTES 4096U
#define VOICE_WSS_STACK_RELEASE_WAIT_MS 2000U

static esp_err_t init_audio_owner(bool mute_after_init)
{
    if (i2s_audio_driver_init_lock == NULL ||
        xSemaphoreTake(i2s_audio_driver_init_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = i2s_audio_driver_init();
    if (err == ESP_OK && mute_after_init) {
        err = i2s_audio_driver_set_output_muted(true);
    }
    xSemaphoreGive(i2s_audio_driver_init_lock);
    return err;
}

#if LUMIDOCK_AUDIO_TEST_ENABLED
static esp_err_t run_microphone_self_test(void)
{
    const size_t pcm_bytes = AUDIO_SELF_TEST_SAMPLES * sizeof(int16_t);
    int16_t *pcm = heap_caps_malloc(pcm_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) return ESP_ERR_NO_MEM;

    const esp_err_t err = i2s_audio_driver_record((uint8_t *)pcm, pcm_bytes, NULL);
    if (err == ESP_OK) {
        int32_t peak = 0;
        int16_t sample_min = INT16_MAX;
        int16_t sample_max = INT16_MIN;
        size_t clipped_samples = 0;
        uint64_t sum_squares = 0;
        for (size_t index = 0; index < AUDIO_SELF_TEST_SAMPLES; ++index) {
            const int32_t sample = pcm[index];
            const int32_t magnitude = sample < 0 ? -sample : sample;
            if (magnitude > peak) peak = magnitude;
            if (sample < sample_min) sample_min = (int16_t)sample;
            if (sample > sample_max) sample_max = (int16_t)sample;
            if (magnitude >= 32760) ++clipped_samples;
            sum_squares += (uint64_t)(sample * sample);
        }
        const float rms = sqrtf((float)(sum_squares / AUDIO_SELF_TEST_SAMPLES));
        ESP_LOGI(TAG, "ES8311 microphone self-test: samples=%u min=%d max=%d peak=%ld rms=%.1f clip=%u%%",
                 (unsigned)AUDIO_SELF_TEST_SAMPLES, sample_min, sample_max, (long)peak,
                 (double)rms, (unsigned)((clipped_samples * 100U) / AUDIO_SELF_TEST_SAMPLES));
    }
    heap_caps_free(pcm);
    return err;
}
#endif

#if !LUMIDOCK_NETWORK_UI_TEST_MODE
static void handle_button_long_press(void)
{
    ui_service_note_activity();
    wifi_service_open_provision_window();
}

static void handle_button_short_press(void)
{
    ui_service_note_activity();
    /* A short press silences a ringing timer (voice or key stop, per design). */
    timer_service_stop_ringing();
}
#endif

static void snapshot_bridge_task(void *arg)
{
    (void)arg;
    (void)watchdog_service_subscribe_current_task();
    for (;;) {
        watchdog_service_feed();
        wifi_service_poll();
        time_service_poll();
        weather_service_poll();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#if LUMIDOCK_AUDIO_TEST_ENABLED
static void early_audio_init_task(void *arg)
{
    (void)arg;
    while (ui_service_boot_is_active()) {
        const esp_err_t err = init_audio_owner(true);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "early ES8311 ready for startup PCM only");
            break;
        }
        ESP_LOGW(TAG, "early ES8311 init: %s; retry in %u ms",
                 esp_err_to_name(err), (unsigned)AUDIO_RETRY_DELAY_MS);
        /* Do not overlap the later PCM self-test/voice initializer if boot
         * completes while this retry delay is pending. */
        for (uint32_t waited_ms = 0U; waited_ms < AUDIO_RETRY_DELAY_MS &&
                                     ui_service_boot_is_active(); waited_ms += 100U) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (!i2s_audio_driver_is_ready()) {
        ESP_LOGI(TAG, "early ES8311 init released at boot end; deferred audio startup owns retries");
    }
    vTaskDelete(NULL);
}

static void voice_services_init_task(void *arg)
{
    (void)arg;
    for (;;) {
        const int64_t release_deadline_us = esp_timer_get_time() +
                                            (int64_t)VOICE_WSS_STACK_RELEASE_WAIT_MS * 1000LL;
        size_t largest_internal = 0U;
        while (esp_timer_get_time() < release_deadline_us) {
            largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            if (largest_internal >= VOICE_WSS_TASK_STACK_BYTES) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "audio startup stack released before WSS init: internal=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)largest_internal);
        const esp_err_t voice_err = xiaozhi_voice_service_init();
        if (voice_err != ESP_OK) {
            ESP_LOGW(TAG, "XiaoZhi voice init: %s; retry in %u ms (internal=%u largest=%u)",
                     esp_err_to_name(voice_err), (unsigned)AUDIO_RETRY_DELAY_MS,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            vTaskDelay(pdMS_TO_TICKS(AUDIO_RETRY_DELAY_MS));
            continue;
        }
        ESP_LOGI(TAG, "ES8311 and XiaoZhi voice services ready");
        break;
    }
    vTaskDelete(NULL);
}

static void audio_startup_task(void *arg)
{
    (void)arg;
    int64_t main_stable_since_us = 0;
    /* Reserve this task's internal stack before Wi-Fi, BLE and HTTP work
     * fragment internal RAM. Audio/model initialization still waits for the
     * same activation and UI/network-settle boundary as before. */
    for (;;) {
        lumidock_snapshot_t snapshot;
        mid_snapshot_get(&snapshot);
        if (snapshot.voice_activation.activated && !snapshot.voice_activation.restart_required &&
            ui_service_is_main_stable()) {
            if (main_stable_since_us == 0) main_stable_since_us = esp_timer_get_time();
            const bool requests_settled = time_service_is_initial_location_settled() &&
                                          weather_service_is_initial_request_settled();
            const bool settle_timeout = esp_timer_get_time() - main_stable_since_us >=
                                        (int64_t)VOICE_INITIAL_NETWORK_SETTLE_TIMEOUT_MS * 1000LL;
            if (requests_settled || settle_timeout) break;
            ESP_LOGI(TAG, "voice startup waiting for initial network requests to settle");
        } else {
            main_stable_since_us = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    ESP_LOGI(TAG, "activation confirmed; starting deferred audio services after main page and initial network settle");
    for (;;) {
        esp_err_t err = init_audio_owner(false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ES8311 audio init: %s; retry in %u ms",
                     esp_err_to_name(err), (unsigned)AUDIO_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(AUDIO_RETRY_DELAY_MS));
            continue;
        }

        err = run_microphone_self_test();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ES8311 microphone self-test: %s; retry in %u ms",
                     esp_err_to_name(err), (unsigned)AUDIO_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(AUDIO_RETRY_DELAY_MS));
            continue;
        }
        break;
    }

    for (;;) {
        if (xTaskCreatePinnedToCoreWithCaps(voice_services_init_task, "voice_services_init", 6144,
                                            NULL, 2, NULL, 1,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
            break;
        }
        ESP_LOGW(TAG, "voice init handoff task unavailable; retry in %u ms",
                 (unsigned)AUDIO_RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(AUDIO_RETRY_DELAY_MS));
    }
    ESP_LOGI(TAG, "audio startup complete; handing voice init to PSRAM task");
    vTaskDelete(NULL);
}
#endif

esp_err_t app_manager_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    err = runtime_event_log_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "runtime event log init failed: %s", esp_err_to_name(err));
    } else {
        (void)runtime_event_log_append(RUNTIME_EVENT_BOOT,
                                       (int32_t)esp_reset_reason(), 0, 0);
        runtime_event_log_dump_recent(16U);
    }
    if ((err = mid_snapshot_init()) != ESP_OK) return err;
    if ((err = watchdog_service_init()) != ESP_OK) {
        ESP_LOGW(TAG, "watchdog init: %s", esp_err_to_name(err));
    }
    /* The boot animation must be running before optional peripherals or
     * networking can perform a slow probe. */
    if ((err = ui_service_init()) != ESP_OK) return err;

#if !LUMIDOCK_NETWORK_UI_TEST_MODE
    if ((err = button_driver_init()) != ESP_OK) return err;
    if ((err = bms_service_init()) != ESP_OK) ESP_LOGW(TAG, "BMS init: %s", esp_err_to_name(err));
#endif
    if ((err = status_led_service_init()) != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 status LED init: %s", esp_err_to_name(err));
    }
    if ((err = startup_coordinator_init()) != ESP_OK) {
        ESP_LOGW(TAG, "startup coordinator init: %s", esp_err_to_name(err));
    }
#if LUMIDOCK_AUDIO_TEST_ENABLED
    i2s_audio_driver_init_lock = xSemaphoreCreateMutex();
    if (i2s_audio_driver_init_lock == NULL) {
        ESP_LOGE(TAG, "audio init ownership mutex unavailable");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(early_audio_init_task, "early_audio_init", 3072,
                                NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "early ES8311 init task unavailable");
    }
    /* Model loading requires an internal stack. Reserve it before later
     * services reduce the largest internal free block; the task itself
     * remains gated on activation and the stable main page. */
    if (xTaskCreatePinnedToCore(audio_startup_task, "audio_startup", 4096, NULL, 2,
                                NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "audio startup task unavailable at early reservation; internal largest=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "audio startup task reserved before network services");
#endif
    if ((err = stc_service_init()) != ESP_OK) {
        ESP_LOGW(TAG, "STC init: %s", esp_err_to_name(err));
    }
    bool wifi_ready = false;
    if ((err = wifi_service_init()) != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi init: %s", esp_err_to_name(err));
        wifi_snapshot_t unavailable = {0};
        unavailable.initialized = true;
        mid_snapshot_set_wifi(&unavailable);
    } else {
        wifi_ready = true;
#if !LUMIDOCK_NETWORK_UI_TEST_MODE
        (void)button_driver_set_long_press_callback(handle_button_long_press);
        (void)button_driver_set_short_press_callback(handle_button_short_press);
#endif
    }
    if ((err = ble_provision_service_init()) != ESP_OK) ESP_LOGW(TAG, "BLE init: %s", esp_err_to_name(err));
    if ((err = time_service_init()) != ESP_OK) ESP_LOGW(TAG, "time init: %s", esp_err_to_name(err));
    if ((err = weather_service_init()) != ESP_OK) ESP_LOGW(TAG, "weather init: %s", esp_err_to_name(err));
    if ((err = xiaozhi_activation_service_init()) != ESP_OK) {
        ESP_LOGW(TAG, "XiaoZhi activation init: %s", esp_err_to_name(err));
    }
    if ((err = radio_stream_service_init()) != ESP_OK) {
        ESP_LOGW(TAG, "radio stream init: %s", esp_err_to_name(err));
    }
#if !LUMIDOCK_AUDIO_TEST_ENABLED
    ESP_LOGI(TAG, "network/UI test profile: deferred audio services disabled");
#endif
    /* Network consumers wait for the public reachability probe, not just DHCP. */
    if (wifi_ready) {
        wifi_service_notify_services_ready();
    }

    if (xTaskCreatePinnedToCoreWithCaps(snapshot_bridge_task, "snapshot_bridge", 3072, NULL, 2, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    mid_snapshot_set_startup_complete(true);

#if LUMIDOCK_NETWORK_UI_TEST_MODE
    ESP_LOGI(TAG, "network/UI test profile started; voice_test=%d", LUMIDOCK_VOICE_TEST_MODE);
#else
    ESP_LOGI(TAG, "LumiDock application started; deferred protocols remain inactive");
#endif
    return ESP_OK;
}
