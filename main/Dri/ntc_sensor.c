#include "ntc_sensor.h"

#include <math.h>
#include <stdint.h>

#include "board_pins.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static const char *TAG = "ntc_sensor";

/* Values printed on the thermistor bag: R25 = 10k, B(25/50) = 3950 K. */
#define NTC_R25_OHMS 10000.0f
#define NTC_BETA_K 3950.0f
#define NTC_T25_KELVIN 298.15f
/* The fixed leg of the divider, same value as R25 so the node sits at half
 * rail at 25 C - the most linear part of the curve. */
#define NTC_SERIES_OHMS 10000.0f
#define NTC_RAIL_MV 3300

/* Averaging window: the oneshot read is cheap, and the thermistor sits next to
 * a switching fan so some noise is expected. */
#define NTC_SAMPLE_COUNT 8
/* The samples have to be spread out in time or the spread check below is blind.
 * Noise is correlated over a few microseconds, so back-to-back conversions of a
 * floating pin look tight; a millisecond apart they show its real scatter. */
#define NTC_SAMPLE_GAP_US 300
/* Anything within this many millivolts of a rail is treated as a broken probe
 * rather than a legitimate reading. */
#define NTC_RAIL_GUARD_MV 10

/* A wire with nothing on the other end is a high impedance antenna: it wanders
 * over hundreds of millivolts, and most of that range sits comfortably between
 * the two rail guards. Without a spread check those readings pass as valid and
 * turn into a confident rubbish temperature. A working divider stays well
 * inside this over the ~2.4 ms the samples span. */
#define NTC_SPREAD_LIMIT_MV 60

/* Both fault paths are polled once a second, so they need their own throttle or
 * a missing probe fills the log. */
#define NTC_FAULT_LOG_INTERVAL_US (10LL * 1000LL * 1000LL)

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_ready;
static int64_t s_fault_log_us;

static bool fault_log_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_fault_log_us < NTC_FAULT_LOG_INTERVAL_US) return false;
    s_fault_log_us = now_us;
    return true;
}

esp_err_t ntc_sensor_init(void)
{
    if (s_ready) return ESP_OK;

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = LUMIDOCK_NTC_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc, LUMIDOCK_NTC_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        (void)adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return err;
    }

    /* Calibration is optional: without it the raw scaling is still good enough
     * to drive a fan curve, so a failure must not take the sensor down.
     *
     * .chan 必须显式写：这个结构体不做范围校验，漏掉就默认通道 0，而校准
     * 曲线是按通道各存一份的。那样采样在 ch2、换算却套 ch0 的曲线，读数会整体
     * 偏一截，而且编译一声不响。 */
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = LUMIDOCK_NTC_ADC_UNIT,
        .chan = LUMIDOCK_NTC_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable; using raw scaling");
        s_cali = NULL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "NTC sensor ready on GPIO%d (ADC1 channel %d)",
             (int)LUMIDOCK_NTC_ADC_GPIO, (int)LUMIDOCK_NTC_ADC_CHANNEL);
    return ESP_OK;
}

bool ntc_sensor_is_ready(void)
{
    return s_ready;
}

static esp_err_t read_millivolts(int *out_mv, int *out_spread_mv)
{
    int32_t sum_mv = 0;
    int valid = 0;
    int min_mv = 0;
    int max_mv = 0;
    for (int index = 0; index < NTC_SAMPLE_COUNT; ++index) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, LUMIDOCK_NTC_ADC_CHANNEL, &raw) != ESP_OK) continue;
        int mv = 0;
        if (s_cali != NULL) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) continue;
        } else {
            mv = (raw * NTC_RAIL_MV) / 4095;
        }
        if (valid == 0 || mv < min_mv) min_mv = mv;
        if (valid == 0 || mv > max_mv) max_mv = mv;
        sum_mv += mv;
        ++valid;
        esp_rom_delay_us(NTC_SAMPLE_GAP_US);
    }
    if (valid == 0) return ESP_FAIL;
    *out_mv = (int)(sum_mv / valid);
    *out_spread_mv = max_mv - min_mv;
    return ESP_OK;
}

esp_err_t ntc_sensor_read_sample(int *out_millivolts, float *out_celsius)
{
    if (!s_ready || (out_millivolts == NULL && out_celsius == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    int mv = 0;
    int spread_mv = 0;
    const esp_err_t err = read_millivolts(&mv, &spread_mv);
    if (err != ESP_OK) return err;

    /* Checked before the rail guards: a floating pin mostly lands *between* the
     * rails, so this is the only guard that catches it. */
    if (spread_mv > NTC_SPREAD_LIMIT_MV) {
        if (fault_log_due()) {
            ESP_LOGW(TAG, "NTC reading unstable: spread %d mV across %d samples (mean %d mV); "
                          "GPIO%d is floating or the probe is disconnected",
                     spread_mv, NTC_SAMPLE_COUNT, mv, (int)LUMIDOCK_NTC_ADC_GPIO);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* A node sitting on either rail means an open or shorted probe. Reporting
     * the corresponding "temperature" would drive the fan to an extreme, so
     * fail instead and let the caller keep its previous decision. */
    if (mv <= NTC_RAIL_GUARD_MV || mv >= NTC_RAIL_MV - NTC_RAIL_GUARD_MV) {
        if (fault_log_due()) {
            ESP_LOGW(TAG, "NTC divider at rail (%d mV); probe open or shorted", mv);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    const float node_v = (float)mv / 1000.0f;
    const float rail_v = (float)NTC_RAIL_MV / 1000.0f;
    const float r_ntc = NTC_SERIES_OHMS * (rail_v - node_v) / node_v;
    const float inv_kelvin = (1.0f / NTC_T25_KELVIN) + (logf(r_ntc / NTC_R25_OHMS) / NTC_BETA_K);

    if (out_millivolts != NULL) *out_millivolts = mv;
    if (out_celsius != NULL) *out_celsius = (1.0f / inv_kelvin) - 273.15f;
    return ESP_OK;
}

esp_err_t ntc_sensor_read_celsius(float *out_celsius)
{
    return ntc_sensor_read_sample(NULL, out_celsius);
}
