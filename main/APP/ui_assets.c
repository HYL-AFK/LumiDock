#include "ui_assets.h"

#include <stdint.h>

extern const uint8_t ui_main_scene_data[] asm("_binary_main_scene_rgb565_start");
extern const uint8_t ui_boot_animation_data[] asm("_binary_boot_animation_gif_start");
extern const uint8_t ui_icon_wifi_data[] asm("_binary_wifi_rgb565_start");
extern const uint8_t ui_icon_bluetooth_data[] asm("_binary_bluetooth_rgb565_start");
extern const uint8_t ui_icon_battery_data[] asm("_binary_battery_rgb565_start");
extern const uint8_t ui_icon_mic_error_data[] asm("_binary_mic_error_rgb565_start");
extern const uint8_t ui_icon_timer_data[] asm("_binary_timer_icon_rgb565a8_start");
extern const uint8_t ui_icon_alarm_data[] asm("_binary_alarm_icon_rgb565a8_start");
extern const uint8_t ui_weather_icon_sunny_data[] asm("_binary_weather_icon_sunny_rgb565a8_start");
extern const uint8_t ui_weather_icon_cloudy_data[] asm("_binary_weather_icon_cloudy_rgb565a8_start");
extern const uint8_t ui_weather_icon_rain_data[] asm("_binary_weather_icon_rain_rgb565a8_start");
extern const uint8_t ui_weather_icon_snow_data[] asm("_binary_weather_icon_snow_rgb565a8_start");
extern const uint8_t ui_weather_clear_scene_data[] asm("_binary_weather_clear_scene_i8_start");
extern const uint8_t ui_weather_cloudy_scene_data[] asm("_binary_weather_cloudy_scene_i8_start");
extern const uint8_t ui_weather_fog_scene_data[] asm("_binary_weather_fog_scene_i8_start");
extern const uint8_t ui_weather_rain_scene_data[] asm("_binary_weather_rain_scene_i8_start");
extern const uint8_t ui_weather_snow_scene_data[] asm("_binary_weather_snow_scene_i8_start");
extern const uint8_t ui_weather_thunder_scene_data[] asm("_binary_weather_thunder_scene_i8_start");
extern const uint8_t ui_voice_connecting_data[] asm("_binary_voice_connecting_i4_start");
extern const uint8_t ui_voice_listening_data[] asm("_binary_voice_listening_i4_start");
extern const uint8_t ui_voice_thinking_data[] asm("_binary_voice_thinking_i4_start");
extern const uint8_t ui_voice_speaking_data[] asm("_binary_voice_speaking_i4_start");
extern const uint8_t ui_voice_error_data[] asm("_binary_voice_error_i4_start");

#define UI_IMAGE(name, source, width, height) \
    const lv_image_dsc_t name = { \
        .header = { \
            .magic = LV_IMAGE_HEADER_MAGIC, \
            .cf = LV_COLOR_FORMAT_RGB565, \
            .w = (width), .h = (height), .stride = (width) * 2U, \
        }, \
        .data_size = (width) * (height) * 2U, \
        .data = (source), \
    }

#define UI_IMAGE_I8(name, source, width, height) \
    const lv_image_dsc_t name = { \
        .header = { \
            .magic = LV_IMAGE_HEADER_MAGIC, \
            .cf = LV_COLOR_FORMAT_I8, \
            .w = (width), .h = (height), .stride = (width), \
        }, \
        .data_size = LV_COLOR_INDEXED_PALETTE_SIZE(LV_COLOR_FORMAT_I8) * sizeof(lv_color32_t) + (width) * (height), \
        .data = (source), \
    }

#define UI_IMAGE_RGB565A8(name, source, width, height) \
    const lv_image_dsc_t name = { \
        .header = { \
            .magic = LV_IMAGE_HEADER_MAGIC, \
            .cf = LV_COLOR_FORMAT_RGB565A8, \
            .w = (width), .h = (height), .stride = (width) * 2U, \
        }, \
        .data_size = (width) * (height) * 3U, \
        .data = (source), \
    }

#define UI_IMAGE_I4(name, source, width, height) \
    const lv_image_dsc_t name = { \
        .header = { \
            .magic = LV_IMAGE_HEADER_MAGIC, \
            .cf = LV_COLOR_FORMAT_I4, \
            .w = (width), .h = (height), .stride = (width) / 2U, \
        }, \
        .data_size = LV_COLOR_INDEXED_PALETTE_SIZE(LV_COLOR_FORMAT_I4) * sizeof(lv_color32_t) + \
                     ((width) * (height)) / 2U, \
        .data = (source), \
    }

UI_IMAGE(ui_main_scene, ui_main_scene_data, 320, 170);
const lv_image_dsc_t ui_boot_animation = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RAW,
        .w = 144,
        .h = 80,
    },
    .data_size = 8340U,
    .data = ui_boot_animation_data,
};
UI_IMAGE(ui_icon_wifi, ui_icon_wifi_data, 24, 24);
UI_IMAGE(ui_icon_bluetooth, ui_icon_bluetooth_data, 24, 24);
UI_IMAGE(ui_icon_battery, ui_icon_battery_data, 24, 24);
UI_IMAGE(ui_icon_mic_error, ui_icon_mic_error_data, 24, 24);
UI_IMAGE_RGB565A8(ui_icon_timer, ui_icon_timer_data, 34, 34);
UI_IMAGE_RGB565A8(ui_icon_alarm, ui_icon_alarm_data, 34, 34);
UI_IMAGE_RGB565A8(ui_weather_icon_sunny, ui_weather_icon_sunny_data, 72, 72);
UI_IMAGE_RGB565A8(ui_weather_icon_cloudy, ui_weather_icon_cloudy_data, 72, 72);
UI_IMAGE_RGB565A8(ui_weather_icon_rain, ui_weather_icon_rain_data, 72, 72);
UI_IMAGE_RGB565A8(ui_weather_icon_snow, ui_weather_icon_snow_data, 72, 72);
UI_IMAGE_I8(ui_weather_clear_scene, ui_weather_clear_scene_data, 320, 170);
UI_IMAGE_I8(ui_weather_cloudy_scene, ui_weather_cloudy_scene_data, 320, 170);
UI_IMAGE_I8(ui_weather_fog_scene, ui_weather_fog_scene_data, 320, 170);
UI_IMAGE_I8(ui_weather_rain_scene, ui_weather_rain_scene_data, 320, 170);
UI_IMAGE_I8(ui_weather_snow_scene, ui_weather_snow_scene_data, 320, 170);
UI_IMAGE_I8(ui_weather_thunder_scene, ui_weather_thunder_scene_data, 320, 170);
UI_IMAGE_I4(ui_voice_connecting, ui_voice_connecting_data, 48, 48);
UI_IMAGE_I4(ui_voice_listening, ui_voice_listening_data, 48, 48);
UI_IMAGE_I4(ui_voice_thinking, ui_voice_thinking_data, 48, 48);
UI_IMAGE_I4(ui_voice_speaking, ui_voice_speaking_data, 48, 48);
UI_IMAGE_I4(ui_voice_error, ui_voice_error_data, 48, 48);
