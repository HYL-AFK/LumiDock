#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Landscape UI geometry for the verified 170x320 panel. */
#define ST7789_WIDTH 320
#define ST7789_HEIGHT 170
#define ST7789_X_OFFSET 0
#define ST7789_Y_OFFSET 35

#define ST7789_COLOR_BLACK 0x0000
#define ST7789_COLOR_WHITE 0xffff
#define ST7789_COLOR_GREEN 0x07e0
#define ST7789_COLOR_YELLOW 0xffe0
#define ST7789_COLOR_CYAN 0x07ff
#define ST7789_COLOR_RED 0xf800
#define ST7789_COLOR_ORANGE 0xfd20
#define ST7789_COLOR_BLUE 0x051f
#define ST7789_COLOR_LIGHT_BLUE 0x7dff
#define ST7789_COLOR_GRAY 0x7bef
#define ST7789_COLOR_DARK_GRAY 0x4208

esp_err_t st7789_init(void);
void st7789_set_backlight(bool enabled);
void st7789_set_backlight_percent(uint8_t percent);
void st7789_fade_backlight_percent(uint8_t percent, uint32_t duration_ms);
void st7789_fill(uint16_t color);
/* Draw RGB565 pixels from a CPU-endian buffer using bounded SPI transfers. */
void st7789_draw_bitmap(int x, int y, int width, int height, const uint16_t *pixels);
