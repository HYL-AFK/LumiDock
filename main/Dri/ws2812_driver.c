#include "ws2812_driver.h"

#include <string.h>

#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"

#include "board_pins.h"

#define WS2812_RESOLUTION_HZ 10000000U
#define WS2812_FRAME_BYTES (LUMIDOCK_WS2812_LED_COUNT * 3U)

static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static bool s_initialized;
static uint8_t s_grb_frame[WS2812_FRAME_BYTES];

static const rmt_symbol_word_t s_zero = {
    .level0 = 1, .duration0 = 3,
    .level1 = 0, .duration1 = 9,
};

static const rmt_symbol_word_t s_one = {
    .level0 = 1, .duration0 = 9,
    .level1 = 0, .duration1 = 3,
};

static const rmt_symbol_word_t s_reset = {
    .level0 = 0, .duration0 = 250,
    .level1 = 0, .duration1 = 250,
};

static size_t ws2812_encode(const void *data, size_t data_size,
                            size_t symbols_written, size_t symbols_free,
                            rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    (void)arg;
    if (symbols_free < 8U) return 0;
    const size_t byte_index = symbols_written / 8U;
    if (byte_index < data_size) {
        const uint8_t value = ((const uint8_t *)data)[byte_index];
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            symbols[bit] = (value & (uint8_t)(0x80U >> bit)) != 0U ? s_one : s_zero;
        }
        return 8U;
    }
    symbols[0] = s_reset;
    *done = true;
    return 1U;
}

esp_err_t ws2812_driver_init(void)
{
    if (s_initialized) return ESP_OK;
    const rmt_tx_channel_config_t channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LUMIDOCK_WS2812_1_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = WS2812_RESOLUTION_HZ,
        .trans_queue_depth = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&channel_config, &s_channel);
    if (err != ESP_OK) return err;
    const rmt_simple_encoder_config_t encoder_config = {
        .callback = ws2812_encode,
    };
    err = rmt_new_simple_encoder(&encoder_config, &s_encoder);
    if (err != ESP_OK) return err;
    err = rmt_enable(s_channel);
    if (err != ESP_OK) return err;
    s_initialized = true;
    ws2812_driver_clear();
    return ESP_OK;
}

esp_err_t ws2812_driver_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    return ws2812_driver_set_all(red, green, blue);
}

esp_err_t ws2812_driver_set_all(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    for (uint8_t index = 0; index < LUMIDOCK_WS2812_LED_COUNT; ++index) {
        const size_t offset = (size_t)index * 3U;
        s_grb_frame[offset] = green;
        s_grb_frame[offset + 1U] = red;
        s_grb_frame[offset + 2U] = blue;
    }
    return ESP_OK;
}

esp_err_t ws2812_driver_set_pixel(uint8_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (index >= LUMIDOCK_WS2812_LED_COUNT) return ESP_ERR_INVALID_ARG;
    const size_t offset = (size_t)index * 3U;
    s_grb_frame[offset] = green;
    s_grb_frame[offset + 1U] = red;
    s_grb_frame[offset + 2U] = blue;
    return ESP_OK;
}

void ws2812_driver_clear(void)
{
    memset(s_grb_frame, 0, sizeof(s_grb_frame));
}

esp_err_t ws2812_driver_show(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    const rmt_transmit_config_t transmit_config = {.loop_count = 0};
    const esp_err_t err = rmt_transmit(s_channel, s_encoder, s_grb_frame, sizeof(s_grb_frame), &transmit_config);
    if (err != ESP_OK) return err;
    return rmt_tx_wait_all_done(s_channel, portMAX_DELAY);
}
