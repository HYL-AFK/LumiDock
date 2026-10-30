#include "i2s_audio_driver.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "i2s_audio";

/* esp_codec_dev stores the ES8311 I2C address in 8-bit wire form (0x30),
 * while the ESP-IDF master probe API takes a 7-bit address (0x18). */
#define ES8311_I2C_ADDRESS_7BIT (ES8311_CODEC_DEFAULT_ADDR >> 1U)
/* The esp_codec_dev es8311 driver writes the raw gain enum (0-7) into the
 * whole ADC_REG16 (0x16) register instead of shifting it into the gain
 * field, which silently leaves the codec at minimum gain. The correct
 * pattern is applied by a direct register write after the codec is open
 * (see below). The dB macro stays for the esp_codec_dev API contract.
 * Field-tested 0x16 patterns:
 *  - 0x24: the driver's own open-time default (~18 dB step), working
 *  - 0x02/0x04: written by the buggy gain path, minimum gain (too quiet)
 *  - 0x28: community table "24 dB" pattern, but the ADC goes fully silent
 *    (bit3 side effect) - do not use. */
/* 麦克风输入增益的接口值；打开编解码器后还要写入校准过的 ADC_REG16。 */
#define ES8311_MIC_INPUT_GAIN_DB 24.0f
#define ES8311_ADC_REG16 0x16
#define ES8311_MIC_GAIN_REG16_VALUE 0x24
/* Requested product playback level; the amplifier path remains unchanged. */
#define ES8311_OUTPUT_VOLUME_PERCENT 4.0f
#define ES8311_MUTE_RETRY_COUNT 3U
#define ES8311_MUTE_RETRY_DELAY_MS 5U
#define ES8311_OUTPUT_PRE_ROLL_BYTES 960U

static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static esp_codec_dev_handle_t s_codec;
static bool s_ready;
static bool s_output_muted = true;
static float s_output_volume_percent = ES8311_OUTPUT_VOLUME_PERCENT;
static SemaphoreHandle_t s_ctrl_lock;
static SemaphoreHandle_t s_rx_lock;
static SemaphoreHandle_t s_tx_lock;
static StaticSemaphore_t s_ctrl_lock_storage;
static StaticSemaphore_t s_rx_lock_storage;
static StaticSemaphore_t s_tx_lock_storage;
static portMUX_TYPE s_lock_mux = portMUX_INITIALIZER_UNLOCKED;
/* Compatibility name retained for existing static contracts; control-only. */
#define s_codec_lock s_ctrl_lock
static uint8_t s_codec_address = ES8311_CODEC_DEFAULT_ADDR;
static uint8_t s_output_pre_roll[ES8311_OUTPUT_PRE_ROLL_BYTES];

static esp_err_t ensure_audio_locks(void)
{
    portENTER_CRITICAL(&s_lock_mux);
    if (s_ctrl_lock == NULL) {
        s_ctrl_lock = xSemaphoreCreateMutexStatic(&s_ctrl_lock_storage);
    }
    if (s_rx_lock == NULL) {
        s_rx_lock = xSemaphoreCreateMutexStatic(&s_rx_lock_storage);
    }
    if (s_tx_lock == NULL) {
        s_tx_lock = xSemaphoreCreateMutexStatic(&s_tx_lock_storage);
    }
    portEXIT_CRITICAL(&s_lock_mux);
    return s_ctrl_lock != NULL && s_rx_lock != NULL && s_tx_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t init_i2c_bus(void)
{
    if (s_i2c_bus != NULL) return ESP_OK;
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = LUMIDOCK_ES8311_I2C_SCL_GPIO,
        .sda_io_num = LUMIDOCK_ES8311_I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

static void release_i2s_channels(void)
{
    if (s_i2s_tx != NULL) {
        (void)i2s_channel_disable(s_i2s_tx);
        (void)i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
    }
    if (s_i2s_rx != NULL) {
        (void)i2s_channel_disable(s_i2s_rx);
        (void)i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
    }
}

static void release_codec(void)
{
    if (s_codec == NULL) return;
    (void)esp_codec_dev_close(s_codec);
    esp_codec_dev_delete(s_codec);
    s_codec = NULL;
    s_output_muted = true;
}

static bool scan_i2c_addresses(uint8_t *candidate_address)
{
    bool found_device = false;

    for (uint8_t address = 0x03U; address <= 0x77U; ++address) {
        if (i2c_master_probe(s_i2c_bus, address, 10) == ESP_OK) {
            ESP_LOGW(TAG, "I2C device found: 0x%02x", address);
            if (!found_device && candidate_address != NULL) *candidate_address = address;
            found_device = true;
        }
    }

    if (!found_device) {
        ESP_LOGW(TAG, "No I2C devices responded on GPIO17/18");
    }
    return found_device;
}

static esp_err_t probe_codec(void)
{
    const esp_err_t err = i2c_master_probe(s_i2c_bus, ES8311_I2C_ADDRESS_7BIT, 100);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "I2C timeout on GPIO17/18; address scan skipped (SDA=%d SCL=%d)",
                 gpio_get_level(LUMIDOCK_ES8311_I2C_SDA_GPIO),
                 gpio_get_level(LUMIDOCK_ES8311_I2C_SCL_GPIO));
        return err;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        uint8_t candidate_address = 0U;
        ESP_LOGI(TAG, "ES8311 not detected on 7-bit I2C 0x%02x", ES8311_I2C_ADDRESS_7BIT);
        if (scan_i2c_addresses(&candidate_address)) {
            s_codec_address = (uint8_t)(candidate_address << 1U);
            ESP_LOGI(TAG, "ES8311 candidate detected at 7-bit I2C 0x%02x", candidate_address);
            return ESP_OK;
        }
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 I2C probe failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t init_i2s_channels(void)
{
    const i2s_chan_config_t channel_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* 采集和播放共用 I2S0：GPIO8 读麦克风，GPIO7 输出到 ES8311 的 DIN。 */
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(LUMIDOCK_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            LUMIDOCK_AUDIO_BITS_PER_SAMPLE, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = LUMIDOCK_I2S_MCLK_GPIO,
            .bclk = LUMIDOCK_I2S_BCLK_GPIO,
            .ws = LUMIDOCK_I2S_LRCLK_GPIO,
            .dout = LUMIDOCK_I2S_AMP_DIN_GPIO,
            .din = LUMIDOCK_I2S_MIC_DATA_GPIO,
        },
    };

    release_i2s_channels();
    esp_err_t err = i2s_new_channel(&channel_cfg, &s_i2s_tx, &s_i2s_rx);
    if (err != ESP_OK) return err;
    if ((err = i2s_channel_init_std_mode(s_i2s_tx, &std_cfg)) != ESP_OK) goto failed;
    if ((err = i2s_channel_init_std_mode(s_i2s_rx, &std_cfg)) != ESP_OK) goto failed;
    if ((err = i2s_channel_enable(s_i2s_tx)) != ESP_OK) goto failed;
    if ((err = i2s_channel_enable(s_i2s_rx)) != ESP_OK) goto failed;
    return ESP_OK;

failed:
    release_i2s_channels();
    return err;
}

static esp_err_t set_codec_output_mute_with_retry(bool muted)
{
    int result = ESP_CODEC_DEV_DRV_ERR;
    for (uint32_t attempt = 1U; attempt <= ES8311_MUTE_RETRY_COUNT; ++attempt) {
        result = esp_codec_dev_set_out_mute(s_codec, muted);
        if (result == ESP_CODEC_DEV_OK) return ESP_OK;
        ESP_LOGW(TAG, "ES8311 output %s I2C operation failed: attempt=%u/%u code=%d",
                 muted ? "mute" : "unmute", (unsigned)attempt,
                 (unsigned)ES8311_MUTE_RETRY_COUNT, result);
        if (attempt < ES8311_MUTE_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(ES8311_MUTE_RETRY_DELAY_MS));
        }
    }
    ESP_LOGE(TAG, "ES8311 output %s failed after %u attempts; I2C/hardware unavailable",
             muted ? "mute" : "unmute", (unsigned)ES8311_MUTE_RETRY_COUNT);
    return ESP_FAIL;
}

esp_err_t i2s_audio_driver_init(void)
{
    if (ensure_audio_locks() != ESP_OK) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_ctrl_lock, portMAX_DELAY);
    if (s_ready) {
        xSemaphoreGive(s_ctrl_lock);
        return ESP_OK;
    }

    esp_err_t err = init_i2c_bus();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 I2C bus init failed: %s", esp_err_to_name(err));
        goto failed_locked;
    }
    if ((err = probe_codec()) != ESP_OK) goto failed_locked;
    if ((err = init_i2s_channels()) != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 I2S init failed: %s", esp_err_to_name(err));
        goto failed_locked;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = s_codec_address,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (data_if == NULL || ctrl_if == NULL || gpio_if == NULL) {
        release_i2s_channels();
        err = ESP_ERR_NO_MEM;
        goto failed_locked;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        /* The NS4150B CTRL signal is not present on this module header.
         * GPIO7 is ES8311 DIN, not a power-amplifier enable line. */
        .pa_pin = GPIO_NUM_NC,
        .use_mclk = true,
        /* 板载 ZTS6216 麦克风接到 ES8311 的 MIC1P，使用模拟输入路径。 */
        .digital_mic = false,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (codec_if == NULL) {
        release_i2s_channels();
        err = ESP_ERR_NO_MEM;
        goto failed_locked;
    }

    esp_codec_dev_cfg_t device_cfg = {
        .codec_if = codec_if,
        .data_if = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
    };
    s_codec = esp_codec_dev_new(&device_cfg);
    if (s_codec == NULL) {
        release_i2s_channels();
        err = ESP_ERR_NO_MEM;
        goto failed_locked;
    }

    esp_codec_dev_sample_info_t pcm_format = {
        .sample_rate = LUMIDOCK_AUDIO_SAMPLE_RATE_HZ,
        .channel = LUMIDOCK_AUDIO_CHANNELS,
        .bits_per_sample = LUMIDOCK_AUDIO_BITS_PER_SAMPLE,
    };
    if ((err = esp_codec_dev_set_out_vol(s_codec, s_output_volume_percent)) != ESP_OK ||
        (err = esp_codec_dev_set_in_gain(s_codec, ES8311_MIC_INPUT_GAIN_DB)) != ESP_OK ||
        (err = esp_codec_dev_open(s_codec, &pcm_format)) != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 open failed: %s", esp_err_to_name(err));
        release_codec();
        release_i2s_channels();
        goto failed_locked;
    }
    /* esp_codec_dev_open re-applies the cached mic gain through
     * es8311_set_mic_gain, which writes the raw enum into ADC_REG16 instead
     * of the gain field; the codec would stay at minimum gain. Override it
     * with the driver's own open-time default pattern (0x24, ~18 dB). */
    if (esp_codec_dev_write_reg(s_codec, ES8311_ADC_REG16, ES8311_MIC_GAIN_REG16_VALUE) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES8311 mic gain register write failed; speech pickup stays at minimum gain");
    }
    if (set_codec_output_mute_with_retry(true) != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 initial output mute failed");
        release_codec();
        release_i2s_channels();
        err = ESP_FAIL;
        goto failed_locked;
    }

    s_output_muted = true;
    s_ready = true;
    ESP_LOGI(TAG, "NS4150B CTRL unavailable on module header; DAC mute and zero PCM keep I2S clock stable");
    ESP_LOGI(TAG, "ES8311 ready: 16 kHz mono PCM, I2C 0x%02x, mic gain %.0f dB, output %.0f%%, muted",
             s_codec_address, (double)ES8311_MIC_INPUT_GAIN_DB,
             (double)ES8311_OUTPUT_VOLUME_PERCENT);
    xSemaphoreGive(s_ctrl_lock);
    return ESP_OK;

failed_locked:
    xSemaphoreGive(s_ctrl_lock);
    return err;
}

bool i2s_audio_driver_is_ready(void)
{
    return s_ready;
}

esp_err_t i2s_audio_driver_set_output_muted(bool muted)
{
    if (!s_ready || s_codec == NULL) return ESP_ERR_INVALID_STATE;
    if (s_ctrl_lock != NULL) xSemaphoreTake(s_ctrl_lock, portMAX_DELAY);
    if (s_output_muted == muted) {
        if (s_ctrl_lock != NULL) xSemaphoreGive(s_ctrl_lock);
        return ESP_OK;
    }
    const esp_err_t err = set_codec_output_mute_with_retry(muted);
    if (err == ESP_OK) {
        s_output_muted = muted;
        ESP_LOGI(TAG, "ES8311 output %s", muted ? "muted" : "enabled for PCM playback");
    }
    if (s_ctrl_lock != NULL) xSemaphoreGive(s_ctrl_lock);
    return err;
}

esp_err_t i2s_audio_driver_set_output_volume(float percent)
{
    if (!s_ready || s_codec == NULL || percent < 0.0f || percent > 100.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctrl_lock != NULL) xSemaphoreTake(s_ctrl_lock, portMAX_DELAY);
    const int result = esp_codec_dev_set_out_vol(s_codec, percent);
    if (result == ESP_CODEC_DEV_OK) {
        s_output_volume_percent = percent;
        ESP_LOGI(TAG, "ES8311 output volume set: %.0f%%", (double)percent);
    }
    if (s_ctrl_lock != NULL) xSemaphoreGive(s_ctrl_lock);
    return result == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

float i2s_audio_driver_get_output_volume(void)
{
    return s_output_volume_percent;
}

esp_err_t i2s_audio_driver_record(uint8_t *pcm, size_t pcm_bytes, size_t *bytes_read)
{
    if (bytes_read != NULL) *bytes_read = 0;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (pcm == NULL || pcm_bytes == 0U) return ESP_ERR_INVALID_ARG;
    if (s_rx_lock != NULL) xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    const int result = esp_codec_dev_read(s_codec, pcm, pcm_bytes);
    if (s_rx_lock != NULL) xSemaphoreGive(s_rx_lock);
    return result == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t i2s_audio_driver_play(const uint8_t *pcm, size_t pcm_bytes, size_t *bytes_written)
{
    if (bytes_written != NULL) *bytes_written = 0;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (pcm == NULL || pcm_bytes == 0U) return ESP_ERR_INVALID_ARG;
    if (s_ctrl_lock != NULL) xSemaphoreTake(s_ctrl_lock, portMAX_DELAY);
    const bool muted = s_output_muted;
    if (s_ctrl_lock != NULL) xSemaphoreGive(s_ctrl_lock);
    if (muted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tx_lock != NULL) xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    const int write_result = esp_codec_dev_write(s_codec, (void *)pcm, pcm_bytes);
    if (s_tx_lock != NULL) xSemaphoreGive(s_tx_lock);
    if (write_result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES8311 PCM write failed: code=%d bytes=%u", write_result,
                 (unsigned)pcm_bytes);
        return ESP_FAIL;
    }
    if (bytes_written != NULL) *bytes_written = pcm_bytes;
    return ESP_OK;
}

esp_err_t i2s_audio_driver_start_output(void)
{
    if (!s_ready || s_codec == NULL) return ESP_ERR_INVALID_STATE;
    if (s_ctrl_lock != NULL) xSemaphoreTake(s_ctrl_lock, portMAX_DELAY);
    const bool muted = s_output_muted;
    if (s_ctrl_lock != NULL) xSemaphoreGive(s_ctrl_lock);
    if (!muted) {
        return ESP_OK;
    }
    if (s_tx_lock != NULL) xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    const int pre_roll_result = esp_codec_dev_write(s_codec, s_output_pre_roll,
                                                    sizeof(s_output_pre_roll));
    if (s_tx_lock != NULL) xSemaphoreGive(s_tx_lock);
    if (pre_roll_result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES8311 silent pre-roll failed: code=%d", pre_roll_result);
        return ESP_FAIL;
    }
    if (s_ctrl_lock != NULL) xSemaphoreTake(s_ctrl_lock, portMAX_DELAY);
    const esp_err_t err = set_codec_output_mute_with_retry(false);
    if (err == ESP_OK) {
        s_output_muted = false;
        ESP_LOGI(TAG, "ES8311 output enabled for PCM playback");
    }
    if (s_ctrl_lock != NULL) xSemaphoreGive(s_ctrl_lock);
    return err;
}

esp_err_t i2s_audio_driver_stop_output(void)
{
    return i2s_audio_driver_set_output_muted(true);
}
