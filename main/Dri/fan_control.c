#include "fan_control.h"

#include <stdint.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "fan_control";

/* Timer 1 / channel 1 are free: the LCD backlight owns timer 0 / channel 0. */
#define FAN_LEDC_MODE LEDC_LOW_SPEED_MODE
#define FAN_LEDC_TIMER LEDC_TIMER_1
#define FAN_LEDC_CHANNEL LEDC_CHANNEL_1
/* 10 bit at 25 kHz needs 3200 source counts per period out of 80 MHz, so the
 * resolution still fits. */
#define FAN_LEDC_RESOLUTION LEDC_TIMER_10_BIT
#define FAN_LEDC_MAX_DUTY ((1U << 10) - 1)

/* A 4-wire fan either stalls or hunts below this duty, so non-zero requests
 * are lifted to it rather than being applied literally. */
#define FAN_MIN_RUNNING_PERCENT 20U

/* FG pulses per revolution, per the 4-wire tachometer standard. */
#define FAN_FG_PULSES_PER_REV 2U
/* Ignore a window shorter than this: too few pulses to compute a stable
 * number, and the caller normally samples once a second anyway. */
#define FAN_RPM_MIN_WINDOW_US 500000LL

/* Counts a 60 mm fan can produce between polls stay far below this; the limit
 * only exists so the hardware counter has a defined wrap point. */
#define FAN_PCNT_HIGH_LIMIT 30000
#define FAN_PCNT_LOW_LIMIT (-30000)

static bool s_ready;
static uint8_t s_percent;
static pcnt_unit_handle_t s_pcnt;
static int64_t s_last_rpm_us;
static uint32_t s_last_rpm;

static esp_err_t init_fg_counter(void)
{
    /* The tachometer is open collector, so it needs a pull-up: without one the
     * line idles low and the counter sees nothing at all. The PCNT driver does
     * not touch pull settings, so configure it here and re-assert it after the
     * channel claims the pin. */
    const gpio_config_t fg_cfg = {
        .pin_bit_mask = 1ULL << LUMIDOCK_FAN_FG_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&fg_cfg);

    const pcnt_unit_config_t unit_cfg = {
        .high_limit = FAN_PCNT_HIGH_LIMIT,
        .low_limit = FAN_PCNT_LOW_LIMIT,
    };
    esp_err_t err = pcnt_new_unit(&unit_cfg, &s_pcnt);
    if (err != ESP_OK) return err;

    const pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = LUMIDOCK_FAN_FG_GPIO,
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t channel = NULL;
    err = pcnt_new_channel(s_pcnt, &chan_cfg, &channel);
    if (err != ESP_OK) return err;

    /* 引脚归 PCNT 管了，上拉要重新声明一次：上面那次 gpio_config 是在通道
     * 接管之前做的，驱动重新配置引脚时不一定保留它。FG 是开漏输出，没上拉
     * 就是浮空输入——实测转速恒为 0、每秒只碰运气抓到 1 个脉冲，就是这个
     * 表现。 */
    (void)gpio_config(&fg_cfg);

    /* Count rising edges only: every pulse on FG is one edge, so holding the
     * count on the falling edge keeps the pulse count exact. */
    err = pcnt_channel_set_edge_action(channel,
                                       PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_HOLD);
    if (err != ESP_OK) return err;

    /* Fan tachometers bounce a little at low speed; 1 us is well under the
     * shortest real pulse and filters that out. */
    const pcnt_glitch_filter_config_t filter_cfg = {.max_glitch_ns = 1000};
    err = pcnt_unit_set_glitch_filter(s_pcnt, &filter_cfg);
    if (err != ESP_OK) return err;

    err = pcnt_unit_enable(s_pcnt);
    if (err != ESP_OK) return err;
    err = pcnt_unit_clear_count(s_pcnt);
    if (err != ESP_OK) return err;
    return pcnt_unit_start(s_pcnt);
}

esp_err_t fan_control_init(void)
{
    if (s_ready) return ESP_OK;

    const ledc_timer_config_t timer_cfg = {
        .speed_mode = FAN_LEDC_MODE,
        .duty_resolution = FAN_LEDC_RESOLUTION,
        .timer_num = FAN_LEDC_TIMER,
        .freq_hz = LUMIDOCK_FAN_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fan PWM timer init failed: %s", esp_err_to_name(err));
        return err;
    }

    const ledc_channel_config_t chan_cfg = {
        .gpio_num = LUMIDOCK_FAN_PWM_GPIO,
        .speed_mode = FAN_LEDC_MODE,
        .channel = FAN_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = FAN_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fan PWM channel init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* The fan is optional hardware: a missing tachometer must not stop the
     * board from booting, so a failure here only costs the RPM readout. */
    err = init_fg_counter();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fan tachometer unavailable: %s; PWM still works",
                 esp_err_to_name(err));
        s_pcnt = NULL;
    }

    s_percent = 0;
    s_last_rpm_us = esp_timer_get_time();
    s_ready = true;
    ESP_LOGI(TAG, "fan ready: PWM GPIO%d @%u Hz, FG GPIO%d",
             (int)LUMIDOCK_FAN_PWM_GPIO, (unsigned)LUMIDOCK_FAN_PWM_FREQ_HZ,
             (int)LUMIDOCK_FAN_FG_GPIO);
    return ESP_OK;
}

bool fan_control_is_ready(void)
{
    return s_ready;
}

esp_err_t fan_control_set_percent(uint8_t percent)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (percent > 100U) percent = 100U;
    if (percent > 0U && percent < FAN_MIN_RUNNING_PERCENT) {
        percent = FAN_MIN_RUNNING_PERCENT;
    }
    if (percent == s_percent) return ESP_OK;

    const uint32_t duty = ((uint32_t)percent * FAN_LEDC_MAX_DUTY + 50U) / 100U;
    esp_err_t err = ledc_set_duty(FAN_LEDC_MODE, FAN_LEDC_CHANNEL, duty);
    if (err != ESP_OK) return err;
    err = ledc_update_duty(FAN_LEDC_MODE, FAN_LEDC_CHANNEL);
    if (err != ESP_OK) return err;

    s_percent = percent;
    ESP_LOGI(TAG, "fan speed %u%% (duty %u)", (unsigned)percent, (unsigned)duty);
    return ESP_OK;
}

uint8_t fan_control_get_percent(void)
{
    return s_percent;
}

uint32_t fan_control_get_rpm(void)
{
    if (!s_ready || s_pcnt == NULL) return 0;

    const int64_t now_us = esp_timer_get_time();
    const int64_t elapsed_us = now_us - s_last_rpm_us;
    if (elapsed_us < FAN_RPM_MIN_WINDOW_US) return s_last_rpm;

    int count = 0;
    if (pcnt_unit_get_count(s_pcnt, &count) != ESP_OK) return s_last_rpm;
    (void)pcnt_unit_clear_count(s_pcnt);
    s_last_rpm_us = now_us;

    if (count <= 0) {
        s_last_rpm = 0;
        return 0;
    }
    /* rpm = pulses / pulses_per_rev / seconds * 60 */
    const uint64_t pulses = (uint64_t)count;
    const uint64_t rpm = (pulses * 60ULL * 1000000ULL) /
                         ((uint64_t)FAN_FG_PULSES_PER_REV * (uint64_t)elapsed_us);
    s_last_rpm = (rpm > UINT32_MAX) ? UINT32_MAX : (uint32_t)rpm;
    return s_last_rpm;
}
