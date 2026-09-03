#include "status_led_service.h"

#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "mid_snapshot.h"
#include "board_pins.h"
#include "watchdog_service.h"
#include "ws2812_driver.h"

#define STATUS_LED_UPDATE_MS 50U
#define STATUS_LED_SUCCESS_BLINKS 2U
#define STATUS_LED_SUCCESS_ON_US 180000LL
#define STATUS_LED_SUCCESS_OFF_US 180000LL
#define STATUS_LED_SUCCESS_US \
    (STATUS_LED_SUCCESS_BLINKS * (STATUS_LED_SUCCESS_ON_US + STATUS_LED_SUCCESS_OFF_US))

typedef enum {
    STATUS_LED_BOOT_ANIMATION = 0,
    STATUS_LED_FAULT,
    STATUS_LED_ACTIVATION,
    STATUS_LED_PROVISIONING,
    STATUS_LED_WIFI_CONNECTING,
    STATUS_LED_VOICE,
    STATUS_LED_WARNING,
    STATUS_LED_CHARGING,
    STATUS_LED_SUCCESS,
    STATUS_LED_NORMAL,
} status_led_state_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} status_led_color_t;

static bool s_initialized;
static volatile bool s_critical_fault;
static volatile bool s_boot_animation_active;
static volatile bool s_user_enabled = true;
static int64_t s_success_until_us;
static int64_t s_success_started_us;
static bool s_internet_was_online;

static uint8_t breathe(uint8_t ceiling, int64_t now_us)
{
    const uint32_t phase = (uint32_t)((now_us / 20000LL) % 100U);
    const uint32_t triangle = phase < 50U ? phase : 100U - phase;
    return (uint8_t)(ceiling / 4U + (ceiling * 3U * triangle) / 200U);
}

static bool double_blink(int64_t now_us)
{
    const uint32_t phase = (uint32_t)((now_us / 1000LL) % 1000U);
    return phase < 90U || (phase >= 180U && phase < 270U);
}

/* Scale a base color channel by a breath alpha in [0,96] so the whole palette
 * shares one brightness ladder while keeping each state's hue distinct. */
static uint8_t color_scale(uint8_t base, uint8_t alpha)
{
    return (uint8_t)(((uint32_t)base * alpha) / 96U);
}

static status_led_state_t choose_state(const lumidock_snapshot_t *snapshot, int64_t now_us)
{
    if (s_critical_fault) return STATUS_LED_FAULT;
    if (s_boot_animation_active) return STATUS_LED_BOOT_ANIMATION;
    if (now_us < s_success_until_us) return STATUS_LED_SUCCESS;
    if (snapshot->voice_activation.activation_code[0] != '\0') return STATUS_LED_ACTIVATION;
    if (snapshot->bluetooth.provisioning_active) return STATUS_LED_PROVISIONING;
    if (snapshot->voice_session.initialized && snapshot->voice_session.active &&
        snapshot->voice_session.state != VOICE_UI_IDLE) {
        return STATUS_LED_VOICE;
    }
    if (snapshot->wifi.connected && !snapshot->wifi.internet_online &&
        (snapshot->wifi.internet_probe_failures > 0U || snapshot->voice_activation.request_failed)) {
        return STATUS_LED_WARNING;
    }
    if (snapshot->wifi.initialized && !snapshot->wifi.connected &&
        snapshot->wifi.sta_reconnect_enabled) {
        return STATUS_LED_WIFI_CONNECTING;
    }
    if (snapshot->bms.online && snapshot->bms.parsed_valid && snapshot->bms.current_ma > 500) {
        return STATUS_LED_CHARGING;
    }
    return STATUS_LED_NORMAL;
}

static void set_pixel(uint8_t index, status_led_color_t color)
{
    (void)ws2812_driver_set_pixel(index, color.red, color.green, color.blue);
}

static void render_success_strip(int64_t now_us)
{
    const int64_t elapsed_us = now_us - s_success_started_us;
    const int64_t phase_us = elapsed_us % (STATUS_LED_SUCCESS_ON_US + STATUS_LED_SUCCESS_OFF_US);
    if (phase_us < STATUS_LED_SUCCESS_ON_US) {
        (void)ws2812_driver_set_all(60U, 255U, 120U);
    } else {
        ws2812_driver_clear();
    }
}

static void render_state(status_led_state_t state, const lumidock_snapshot_t *snapshot, int64_t now_us)
{
    ws2812_driver_clear();
    if (!s_user_enabled) return;
    switch (state) {
    case STATUS_LED_BOOT_ANIMATION: {
        const uint8_t head = (uint8_t)((now_us / 70000LL) % LUMIDOCK_WS2812_LED_COUNT);
        for (uint8_t tail = 0U; tail < 7U; ++tail) {
            const uint8_t index = (uint8_t)((head + LUMIDOCK_WS2812_LED_COUNT - tail) %
                                            LUMIDOCK_WS2812_LED_COUNT);
            const uint8_t level = (uint8_t)(112U - tail * 14U);
            set_pixel(index, (status_led_color_t){0U, level, (uint8_t)(level * 168U / 112U)});
        }
        break;
    }
    case STATUS_LED_FAULT:
        if (double_blink(now_us)) (void)ws2812_driver_set_all(255U, 40U, 40U);
        break;
    case STATUS_LED_ACTIVATION: {
        const uint8_t k = breathe(110U, now_us);
        (void)ws2812_driver_set_all(color_scale(255U, k), color_scale(200U, k), color_scale(60U, k));
        break;
    }
    case STATUS_LED_PROVISIONING: {
        const uint8_t step = (uint8_t)((now_us / 90000LL) % (LUMIDOCK_WS2812_LED_COUNT * 2U - 2U));
        const uint8_t index = step < LUMIDOCK_WS2812_LED_COUNT ? step : (uint8_t)(LUMIDOCK_WS2812_LED_COUNT * 2U - 2U - step);
        set_pixel(index, (status_led_color_t){0U, 190U, 210U});
        break;
    }
    case STATUS_LED_WIFI_CONNECTING:
        set_pixel((uint8_t)((now_us / 70000LL) % LUMIDOCK_WS2812_LED_COUNT), (status_led_color_t){0U, 110U, 255U});
        break;
    case STATUS_LED_VOICE: {
        const uint8_t center = LUMIDOCK_WS2812_LED_COUNT / 2U;
        uint8_t r = 0U, g = 150U, b = 255U, reach = 2U;
        switch (snapshot->voice_session.state) {
        case VOICE_UI_THINKING:
            r = 230U; g = 180U; b = 80U; reach = 2U; break;
        case VOICE_UI_SPEAKING:
            r = 200U; g = 90U; b = 255U; reach = 3U; break;
        case VOICE_UI_LISTENING:
        default:
            r = 0U; g = 150U; b = 255U; reach = 2U; break;
        }
        const uint8_t k = breathe(96U, now_us);
        for (uint8_t off = 0U; off < reach; ++off) {
            const uint8_t amp = (uint8_t)(96U - off * 24U);
            const uint8_t alpha = (uint8_t)(((uint32_t)k * amp) / 96U);
            const status_led_color_t c = {color_scale(r, alpha), color_scale(g, alpha), color_scale(b, alpha)};
            set_pixel((uint8_t)(center - 1U - off), c);
            set_pixel((uint8_t)(center + off), c);
        }
        break;
    }
    case STATUS_LED_WARNING:
        if (double_blink(now_us)) (void)ws2812_driver_set_all(255U, 140U, 0U);
        break;
    case STATUS_LED_CHARGING: {
        const uint8_t filled = (uint8_t)(((uint32_t)snapshot->bms.soc_percent * LUMIDOCK_WS2812_LED_COUNT + 99U) / 100U);
        const uint8_t k = breathe(96U, now_us);
        for (uint8_t index = 0; index < filled && index < LUMIDOCK_WS2812_LED_COUNT; ++index) {
            set_pixel(index, (status_led_color_t){color_scale(60U, k), color_scale(230U, k), color_scale(120U, k)});
        }
        break;
    }
    case STATUS_LED_SUCCESS:
        render_success_strip(now_us);
        break;
    case STATUS_LED_NORMAL:
    default: {
        const uint8_t k = breathe(36U, now_us);
        const status_led_color_t idle = {color_scale(28U, k), color_scale(34U, k), color_scale(42U, k)};
        for (uint8_t i = 0U; i < LUMIDOCK_WS2812_LED_COUNT; ++i)
            set_pixel(i, idle);
        break;
    }
    }
}

static void status_led_task(void *arg)
{
    (void)arg;
    (void)watchdog_service_subscribe_current_task();
    for (;;) {
        watchdog_service_feed();
        const int64_t now_us = esp_timer_get_time();
        const lumidock_snapshot_t *snapshot = NULL;
        if (mid_snapshot_read_acquire(&snapshot)) {
            if (snapshot->wifi.internet_online && !s_internet_was_online) {
                s_success_started_us = now_us;
                s_success_until_us = now_us + STATUS_LED_SUCCESS_US;
            }
            s_internet_was_online = snapshot->wifi.internet_online;
            render_state(choose_state(snapshot, now_us), snapshot, now_us);
            (void)ws2812_driver_show();
            mid_snapshot_read_release();
        }
        vTaskDelay(pdMS_TO_TICKS(STATUS_LED_UPDATE_MS));
    }
}

esp_err_t status_led_service_init(void)
{
    if (s_initialized) return ESP_OK;
    esp_err_t err = ws2812_driver_init();
    if (err != ESP_OK) return err;
    if (xTaskCreatePinnedToCoreWithCaps(status_led_task, "status_led", 3072, NULL, 1, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI("status_led", "WS2812 status service started on GPIO13");
    return ESP_OK;
}

void status_led_service_set_critical_fault(bool active)
{
    s_critical_fault = active;
}

esp_err_t status_led_service_set_user_enabled(bool enabled)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    s_user_enabled = enabled;
    ESP_LOGI("status_led", "user light-strip override: %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

void status_led_service_set_boot_animation_active(bool active)
{
    s_boot_animation_active = active;
}
