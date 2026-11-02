#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
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

/* DS3231 走独立总线（I2C_NUM_1），不和 ES8311 那条共用。
 *
 * 理论上挂在 GPIO17/18 上就行——I2C 本来就是多设备总线，0x18 和 0x68 也不
 * 打架。之所以单开一条，是因为那两个脚已经焊死在音频那侧，再并一路要么飞线
 * 要么重做板子；而 GPIO21/47 是剩下的空闲脚里最干净的一对：不是 strapping，
 * 不是 JTAG，没被 flash/PSRAM 占，Quad PSRAM 模式下也不受限。
 *
 * 注：具体在第几个焊盘以实际板型为准（WROOM-1 与 Zero 的脚位排布不同），
 * 这里只约定 GPIO 编号，不约定物理位置。
 *
 * 只用 SDA/SCL 两根。DS3231 的 SQW/INT 不接：时间是靠 time_service_poll()
 * 那个 1 Hz 的拍子去读的，不需要硬件中断唤醒。 */
#define LUMIDOCK_DS3231_I2C_PORT I2C_NUM_1
#define LUMIDOCK_DS3231_I2C_SDA_GPIO GPIO_NUM_21
#define LUMIDOCK_DS3231_I2C_SCL_GPIO GPIO_NUM_47
#define LUMIDOCK_DS3231_I2C_ADDRESS 0x68U
#define LUMIDOCK_DS3231_I2C_CLOCK_HZ (400 * 1000)

/* 模块还没到手，这里只把引脚占下来，驱动先不参与编译。
 *
 * 置 0 时 Dri/ds3231.c 和 APP/app_rtc.c 退化成空实现，一个字节的 I2C 事务
 * 都不会发，串口也不会多出半行日志。买回模块、按上面的引脚接好之后把它改成
 * 1，整套逻辑（开机读芯片、SNTP/HTTP 校时后写回 UTC、OSF 有效性检查）直接
 * 生效，不需要再动别的地方。 */
#define LUMIDOCK_DS3231_ENABLE 0

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
