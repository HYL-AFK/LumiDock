#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/spi_master.h"
#include "hal/adc_types.h"

/* LumiDock ESP32-S3-Zero board contract. Keep all GPIO decisions here. */
#define LUMIDOCK_WS2812_1_GPIO GPIO_NUM_13
#define LUMIDOCK_WS2812_2_GPIO GPIO_NUM_14
#define LUMIDOCK_WS2812_LED_COUNT 24U

/* On-board single WS2812B status LED (U1 XL-5050RGBC-WS2812B), driven via RMT.
 * This is independent of the GPIO13/14 strip above (separate DI data line). */
#define LUMIDOCK_ONBOARD_LED_GPIO GPIO_NUM_48

#define LUMIDOCK_I2S_BCLK_GPIO GPIO_NUM_10
#define LUMIDOCK_I2S_LRCLK_GPIO GPIO_NUM_9
#define LUMIDOCK_I2S_MIC_DATA_GPIO GPIO_NUM_8
#define LUMIDOCK_I2S_AMP_DIN_GPIO GPIO_NUM_7
#define LUMIDOCK_I2S_MCLK_GPIO GPIO_NUM_38

#define LUMIDOCK_ES8311_I2C_SDA_GPIO GPIO_NUM_17
#define LUMIDOCK_ES8311_I2C_SCL_GPIO GPIO_NUM_18

#define LUMIDOCK_LCD_HOST SPI2_HOST
/* Ordered to match the user's 1.9-inch TFT header after the power pins:
 * SCL, SDA, RES, DC, CS, BLK. */
#define LUMIDOCK_LCD_SCLK_GPIO GPIO_NUM_1
#define LUMIDOCK_LCD_MOSI_GPIO GPIO_NUM_2
#define LUMIDOCK_LCD_RST_GPIO GPIO_NUM_11
#define LUMIDOCK_LCD_DC_GPIO GPIO_NUM_4
#define LUMIDOCK_LCD_CS_GPIO GPIO_NUM_5
#define LUMIDOCK_LCD_BL_GPIO GPIO_NUM_6

#define LUMIDOCK_BUTTON_GPIO GPIO_NUM_12

/* NTC thermistor (10k, B=3950) on the only free ADC1 channel.
 * Divider is VCC -- NTC -- node -- 10k -- GND, so the node rises with
 * temperature. That polarity matters: GPIO3 is a strapping pin (JTAG source
 * select), and this way the pin stays high at reset instead of being pulled
 * low by a hot thermistor. */
#define LUMIDOCK_NTC_ADC_GPIO GPIO_NUM_3
#define LUMIDOCK_NTC_ADC_CHANNEL ADC_CHANNEL_2
#define LUMIDOCK_NTC_ADC_UNIT ADC_UNIT_1

/* 4-wire fan. PWM takes the Intel 25 kHz control input, FG is the open
 * collector tachometer output that pulses twice per revolution. */
#define LUMIDOCK_FAN_PWM_GPIO GPIO_NUM_15
#define LUMIDOCK_FAN_FG_GPIO GPIO_NUM_16
#define LUMIDOCK_FAN_PWM_FREQ_HZ 25000U

#define LUMIDOCK_BMS_UART_PORT UART_NUM_2
#define LUMIDOCK_BMS_TX_GPIO GPIO_NUM_39
#define LUMIDOCK_BMS_RX_GPIO GPIO_NUM_40
#define LUMIDOCK_BMS_DE_RE_GPIO GPIO_NUM_NC

/* STC8H link: top-board UART pins. UART0 is reserved for this link;
 * console output is routed through USB Serial JTAG in sdkconfig. */
#define LUMIDOCK_STC_UART_PORT UART_NUM_0
#define LUMIDOCK_STC_TX_GPIO GPIO_NUM_43
#define LUMIDOCK_STC_RX_GPIO GPIO_NUM_44

/* LCD SPI clock for the smooth startup animation and page transition. */
#define LUMIDOCK_LCD_SPI_CLOCK_HZ (30 * 1000 * 1000)
#define LUMIDOCK_I2C_CLOCK_HZ (400 * 1000)
