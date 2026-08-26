#include "st7789_driver.h"

#include <string.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "board_pins.h"

static spi_device_handle_t s_device;
static bool s_initialized;
static bool s_backlight_pwm_ready;
static const char *TAG = "st7789";
#define ST7789_FILL_STRIPE_ROWS 8U
static uint8_t s_fill_buffer[ST7789_WIDTH * ST7789_FILL_STRIPE_ROWS * 2U];

static esp_err_t backlight_pwm_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 4000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;
    const ledc_channel_config_t channel = {
        .gpio_num = LUMIDOCK_LCD_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) return err;
    err = ledc_fade_func_install(0);
    if (err == ESP_OK) s_backlight_pwm_ready = true;
    return err;
}

void st7789_set_backlight_percent(uint8_t percent)
{
    if (percent > 100U) percent = 100U;
    if (!s_backlight_pwm_ready) {
        (void)gpio_set_level(LUMIDOCK_LCD_BL_GPIO, percent == 0U ? 0 : 1);
        return;
    }
    const uint32_t duty = ((uint32_t)percent * 255U + 50U) / 100U;
    (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void st7789_set_backlight(bool enabled)
{
    st7789_set_backlight_percent(enabled ? 100U : 0U);
}

void st7789_fade_backlight_percent(uint8_t percent, uint32_t duration_ms)
{
    if (percent > 100U) percent = 100U;
    if (!s_backlight_pwm_ready || duration_ms == 0U) {
        st7789_set_backlight_percent(percent);
        return;
    }
    const uint32_t duty = ((uint32_t)percent * 255U + 50U) / 100U;
    if (ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty,
                                (int)duration_ms) != ESP_OK ||
        ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_NO_WAIT) != ESP_OK) {
        st7789_set_backlight_percent(percent);
    }
}

static esp_err_t write_bytes(const uint8_t *data, size_t length)
{
    if (s_device == NULL) return ESP_ERR_INVALID_STATE;
    spi_transaction_t transaction = {
        .length = length * 8U,
        .tx_buffer = data,
    };
    const esp_err_t err = spi_device_transmit(s_device, &transaction);
    if (err != ESP_OK) ESP_LOGE(TAG, "SPI transmit failed: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t write_command(uint8_t command)
{
    (void)gpio_set_level(LUMIDOCK_LCD_DC_GPIO, 0);
    return write_bytes(&command, 1);
}

static esp_err_t write_data(const uint8_t *data, size_t length)
{
    (void)gpio_set_level(LUMIDOCK_LCD_DC_GPIO, 1);
    return write_bytes(data, length);
}

static esp_err_t write_command_data(uint8_t command, const uint8_t *data, size_t length)
{
    esp_err_t err = write_command(command);
    if (err == ESP_OK && data != NULL && length > 0U) err = write_data(data, length);
    return err;
}

static esp_err_t set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t data[4];
    x0 = (uint16_t)(x0 + ST7789_X_OFFSET);
    x1 = (uint16_t)(x1 + ST7789_X_OFFSET);
    y0 = (uint16_t)(y0 + ST7789_Y_OFFSET);
    y1 = (uint16_t)(y1 + ST7789_Y_OFFSET);
    esp_err_t err = write_command(0x2a);
    data[0] = x0 >> 8; data[1] = x0 & 0xff; data[2] = x1 >> 8; data[3] = x1 & 0xff;
    if (err == ESP_OK) err = write_data(data, sizeof(data));
    if (err == ESP_OK) err = write_command(0x2b);
    data[0] = y0 >> 8; data[1] = y0 & 0xff; data[2] = y1 >> 8; data[3] = y1 & 0xff;
    if (err == ESP_OK) err = write_data(data, sizeof(data));
    if (err == ESP_OK) err = write_command(0x2c);
    return err;
}

esp_err_t st7789_init(void)
{
    if (s_initialized) return ESP_OK;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LUMIDOCK_LCD_DC_GPIO) |
                        (1ULL << LUMIDOCK_LCD_RST_GPIO) |
                        (1ULL << LUMIDOCK_LCD_BL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
    err = gpio_set_direction(LUMIDOCK_LCD_BL_GPIO, GPIO_MODE_INPUT_OUTPUT);
    if (err != ESP_OK) return err;
    err = gpio_set_level(LUMIDOCK_LCD_BL_GPIO, 0);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "LCD backlight GPIO%d: held LOW until the LVGL startup frame is ready, readback=%d",
             LUMIDOCK_LCD_BL_GPIO, gpio_get_level(LUMIDOCK_LCD_BL_GPIO));
    err = backlight_pwm_init();
    if (err != ESP_OK) return err;
    spi_bus_config_t bus = {
        .mosi_io_num = LUMIDOCK_LCD_MOSI_GPIO,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = LUMIDOCK_LCD_SCLK_GPIO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = ST7789_WIDTH * ST7789_FILL_STRIPE_ROWS * 2,
    };
    err = spi_bus_initialize(LUMIDOCK_LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    spi_device_interface_config_t device = {
        .clock_speed_hz = LUMIDOCK_LCD_SPI_CLOCK_HZ,
        .mode = 3,
        .spics_io_num = LUMIDOCK_LCD_CS_GPIO,
        .queue_size = 1,
    };
    err = spi_bus_add_device(LUMIDOCK_LCD_HOST, &device, &s_device);
    if (err != ESP_OK) return err;
    if ((err = gpio_set_level(LUMIDOCK_LCD_RST_GPIO, 0)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));
    if ((err = gpio_set_level(LUMIDOCK_LCD_RST_GPIO, 1)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));
#define ST7789_INIT_WRITE(expression) \
    do { \
        err = (expression); \
        if (err != ESP_OK) return err; \
    } while (0)

    ST7789_INIT_WRITE(write_command(0x11)); /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Values transcribed from the module vendor's STM32 reference program. */
    const uint8_t format = 0x05; /* RGB565 */
    const uint8_t c5[] = {0x1a};
    const uint8_t orientation = 0x60; /* landscape 320x170 */
    const uint8_t porch[] = {0x05, 0x05, 0x00, 0x33, 0x33};
    const uint8_t gate[] = {0x05};
    const uint8_t vcom[] = {0x3f};
    const uint8_t lcm[] = {0x2c};
    const uint8_t vdv[] = {0x01};
    const uint8_t vrhs[] = {0x0f};
    const uint8_t vrls[] = {0x20};
    const uint8_t frctrl[] = {0x01};
    const uint8_t power[] = {0xa4, 0xa1};
    const uint8_t e8[] = {0x03};
    const uint8_t e9[] = {0x09, 0x09, 0x08};
    const uint8_t gamma_pos[] = {
        0xd0, 0x05, 0x09, 0x09, 0x08, 0x14, 0x28,
        0x33, 0x3f, 0x07, 0x13, 0x14, 0x28, 0x30,
    };
    const uint8_t gamma_neg[] = {
        0xd0, 0x05, 0x09, 0x09, 0x08, 0x03, 0x24,
        0x32, 0x32, 0x3b, 0x14, 0x13, 0x28, 0x2f,
    };
    ST7789_INIT_WRITE(write_command_data(0x3a, &format, 1));
    ST7789_INIT_WRITE(write_command_data(0xc5, c5, sizeof(c5)));
    ST7789_INIT_WRITE(write_command_data(0x36, &orientation, 1));
    ST7789_INIT_WRITE(write_command_data(0xb2, porch, sizeof(porch)));
    ST7789_INIT_WRITE(write_command_data(0xb7, gate, sizeof(gate)));
    ST7789_INIT_WRITE(write_command_data(0xbb, vcom, sizeof(vcom)));
    ST7789_INIT_WRITE(write_command_data(0xc0, lcm, sizeof(lcm)));
    ST7789_INIT_WRITE(write_command_data(0xc2, vdv, sizeof(vdv)));
    ST7789_INIT_WRITE(write_command_data(0xc3, vrhs, sizeof(vrhs)));
    ST7789_INIT_WRITE(write_command_data(0xc4, vrls, sizeof(vrls)));
    ST7789_INIT_WRITE(write_command_data(0xc6, frctrl, sizeof(frctrl)));
    ST7789_INIT_WRITE(write_command_data(0xd0, power, sizeof(power)));
    ST7789_INIT_WRITE(write_command_data(0xe8, e8, sizeof(e8)));
    ST7789_INIT_WRITE(write_command_data(0xe9, e9, sizeof(e9)));
    ST7789_INIT_WRITE(write_command_data(0xe0, gamma_pos, sizeof(gamma_pos)));
    ST7789_INIT_WRITE(write_command_data(0xe1, gamma_neg, sizeof(gamma_neg)));
    ST7789_INIT_WRITE(write_command(0x21)); /* INVON */
    ST7789_INIT_WRITE(write_command(0x29)); /* DISPON */
#undef ST7789_INIT_WRITE
    vTaskDelay(pdMS_TO_TICKS(100));
    s_initialized = true;
    st7789_fill(ST7789_COLOR_BLACK);
    return ESP_OK;
}

void st7789_fill(uint16_t color)
{
    if (!s_initialized) return;
    if (set_window(0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1) != ESP_OK) return;
    for (size_t pixel = 0; pixel < ST7789_WIDTH * ST7789_FILL_STRIPE_ROWS; ++pixel) {
        s_fill_buffer[pixel * 2] = (uint8_t)(color >> 8);
        s_fill_buffer[pixel * 2 + 1] = (uint8_t)color;
    }
    for (int y = 0; y < ST7789_HEIGHT; y += ST7789_FILL_STRIPE_ROWS) {
        const int rows = (ST7789_HEIGHT - y) < ST7789_FILL_STRIPE_ROWS ?
                         (ST7789_HEIGHT - y) : ST7789_FILL_STRIPE_ROWS;
        (void)write_data(s_fill_buffer, (size_t)ST7789_WIDTH * rows * 2U);
    }
}

void st7789_draw_bitmap(int x, int y, int width, int height, const uint16_t *pixels)
{
    if (!s_initialized || pixels == NULL || width <= 0 || height <= 0) return;
    if (x < 0 || y < 0 || x + width > ST7789_WIDTH || y + height > ST7789_HEIGHT) {
        ESP_LOGW(TAG, "bitmap outside panel: x=%d y=%d w=%d h=%d", x, y, width, height);
        return;
    }

    /* The controller auto-increments the address window. Set it once for the
     * complete LVGL area; repeating the command sequence for every DMA stripe
     * adds dozens of blocking SPI transactions to each GIF frame. */
    if (set_window((uint16_t)x, (uint16_t)y,
                   (uint16_t)(x + width - 1),
                   (uint16_t)(y + height - 1)) != ESP_OK) {
        return;
    }
    for (int row = 0; row < height; row += ST7789_FILL_STRIPE_ROWS) {
        const int rows = (height - row) < ST7789_FILL_STRIPE_ROWS ?
                             (height - row) : ST7789_FILL_STRIPE_ROWS;
        const size_t pixel_count = (size_t)width * (size_t)rows;
        for (size_t index = 0; index < pixel_count; ++index) {
            const uint16_t color = pixels[(size_t)row * (size_t)width + index];
            s_fill_buffer[index * 2U] = (uint8_t)(color >> 8);
            s_fill_buffer[index * 2U + 1U] = (uint8_t)color;
        }
        (void)write_data(s_fill_buffer, pixel_count * 2U);
    }
}
