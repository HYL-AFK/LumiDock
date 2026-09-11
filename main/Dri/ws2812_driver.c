#include "ws2812_driver.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_private/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/gpio_pins.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "soc/spi_periph.h"

#include "board_pins.h"

/*
 * WS2812 accepts one bit in 1.25 us. SPI3 runs at 2.5 MHz and represents
 * each WS2812 bit with three SPI bits: 100 for zero and 110 for one. The
 * same SPI3 D output signal is routed to GPIO13 and GPIO14, so both strips
 * receive the exact same frame without a second renderer or a second clock.
 */
#define WS2812_SPI_HOST SPI3_HOST
#define WS2812_SPI_CLOCK_HZ 2500000U
#define WS2812_RESET_US 80U
#define WS2812_FRAME_BYTES (LUMIDOCK_WS2812_LED_COUNT * 3U)
#define WS2812_ENCODED_DATA_BYTES (WS2812_FRAME_BYTES * 3U)
#define WS2812_RESET_BYTES 16U
#define WS2812_ENCODED_BYTES (WS2812_ENCODED_DATA_BYTES + WS2812_RESET_BYTES)

static const char *TAG = "ws2812";
static spi_device_handle_t s_device;
static bool s_initialized;
static uint8_t s_grb_frame[WS2812_FRAME_BYTES];
static DMA_ATTR uint8_t s_encoded_frame[WS2812_ENCODED_BYTES];
static const gpio_num_t s_output_gpios[] = {
    LUMIDOCK_WS2812_1_GPIO,
    LUMIDOCK_WS2812_2_GPIO,
};

static void connect_output_gpio(gpio_num_t gpio)
{
    (void)gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    (void)gpio_func_sel(gpio, PIN_FUNC_GPIO);
    esp_rom_gpio_connect_out_signal(gpio, spi_periph_signal[WS2812_SPI_HOST].spid_out,
                                    false, false);
}

static void hold_outputs_low(void)
{
    /* Disconnect the peripheral output during the reset interval. */
    for (size_t index = 0U; index < sizeof(s_output_gpios) / sizeof(s_output_gpios[0]); ++index) {
        esp_rom_gpio_connect_out_signal(s_output_gpios[index], SIG_GPIO_OUT_IDX, false, false);
        (void)gpio_set_level(s_output_gpios[index], 0);
    }
    esp_rom_delay_us(WS2812_RESET_US);
    connect_output_gpio(LUMIDOCK_WS2812_1_GPIO);
    connect_output_gpio(LUMIDOCK_WS2812_2_GPIO);
}

static void encode_frame(void)
{
    size_t output_bit = 0U;
    memset(s_encoded_frame, 0, sizeof(s_encoded_frame));

    for (size_t byte_index = 0U; byte_index < sizeof(s_grb_frame); ++byte_index) {
        const uint8_t value = s_grb_frame[byte_index];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint8_t code = (value & (uint8_t)(0x80U >> bit)) != 0U ? 0x6U : 0x4U;
            for (int8_t code_bit = 2; code_bit >= 0; --code_bit) {
                if ((code & (uint8_t)(1U << code_bit)) != 0U) {
                    s_encoded_frame[output_bit / 8U] |=
                        (uint8_t)(0x80U >> (output_bit % 8U));
                }
                ++output_bit;
            }
        }
    }
}

esp_err_t ws2812_driver_init(void)
{
    if (s_initialized) return ESP_OK;

    const spi_bus_config_t bus_config = {
        .mosi_io_num = LUMIDOCK_WS2812_1_GPIO,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = WS2812_ENCODED_BYTES,
    };
    esp_err_t err = spi_bus_initialize(WS2812_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = WS2812_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(WS2812_SPI_HOST, &device_config, &s_device);
    if (err != ESP_OK) {
        (void)spi_bus_free(WS2812_SPI_HOST);
        return err;
    }

    connect_output_gpio(LUMIDOCK_WS2812_1_GPIO);
    connect_output_gpio(LUMIDOCK_WS2812_2_GPIO);
    s_initialized = true;
    ws2812_driver_clear();
    ESP_LOGI(TAG, "SPI3 WS2812 outputs initialized on GPIO%d and GPIO%d (synchronized)",
             LUMIDOCK_WS2812_1_GPIO, LUMIDOCK_WS2812_2_GPIO);
    return ESP_OK;
}

esp_err_t ws2812_driver_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    return ws2812_driver_set_all(red, green, blue);
}

esp_err_t ws2812_driver_set_all(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    for (uint8_t index = 0U; index < LUMIDOCK_WS2812_LED_COUNT; ++index) {
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
    if (!s_initialized || s_device == NULL) return ESP_ERR_INVALID_STATE;
    encode_frame();
    spi_transaction_t transaction = {
        .length = WS2812_ENCODED_BYTES * 8U,
        .tx_buffer = s_encoded_frame,
    };
    const esp_err_t err = spi_device_transmit(s_device, &transaction);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI3 WS2812 transmit failed: %s", esp_err_to_name(err));
        hold_outputs_low();
        return err;
    }
    /* The trailing zero bytes normally provide reset; keep an explicit guard
     * interval so the latch timing remains valid if the clock is adjusted. */
    esp_rom_delay_us(WS2812_RESET_US);
    return ESP_OK;
}
