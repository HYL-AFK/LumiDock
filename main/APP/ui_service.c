#include "ui_service.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "mid_snapshot.h"
#include "st7789_driver.h"
#include "ui_assets.h"
#include "ui_cjk_font.h"
#include "ui_time_font.h"
#include "watchdog_service.h"

#define LUMIDOCK_LVGL_BUFFER_ROWS 8U
#define UI_STANDBY_TIMEOUT_US (264000000LL)
#define UI_CHARGE_ENTER_MA 500
#define UI_CHARGE_EXIT_MA 300
#define UI_CHARGE_EXIT_HOLD_US 3000000LL
#define UI_BOOT_MIN_DURATION_US (10000000LL)
#define UI_BOOT_MAX_DURATION_US (15000000LL)
#define UI_BOOT_SOUND_LEAD_US (4000000LL)
#define UI_ACTIVATION_RESTART_DELAY_US (1000LL * 1000LL)
#define UI_MAIN_STABLE_DELAY_US (1000LL * 1000LL)
#define UI_WEATHER_LOADING_TIMEOUT_US (10000000LL)
#define UI_BOOT_FADE_OUT_US (80000LL)
#define UI_BOOT_FADE_IN_US (160000LL)
#define UI_STANDBY_EXPRESSION_PERIOD_US (3200000LL)
#define UI_STANDBY_EYE_OPEN_WIDTH 76
#define UI_STANDBY_EYE_OPEN_HEIGHT 68
#define UI_STANDBY_EYE_CLOSED_WIDTH 64
#define UI_STANDBY_EYE_CLOSED_HEIGHT 12
#define UI_STANDBY_EYE_LEFT_CENTER 105
#define UI_STANDBY_EYE_RIGHT_CENTER 215
#define UI_STANDBY_EYE_CENTER_Y 85

/* UI colors are passed through color(), which accepts RGB565 values. */
enum { UI_BACKGROUND = 0xffff, UI_INK = 0x2104, UI_MUTED = 0x6b6f, UI_ACCENT = 0x354d };

#define UI_MAIN_DATE_Y 90
#define UI_MAIN_WEATHER_GAP 4
#define UI_MAIN_RIGHT_MARGIN 13

typedef enum {
    UI_PAGE_BOOT = 0,
    UI_PAGE_MAIN,
    UI_PAGE_CHARGING,
    UI_PAGE_STANDBY,
    UI_PAGE_ACTIVATION,
    UI_PAGE_VOICE,
} ui_page_t;

typedef enum {
    UI_BOOT_TRANSITION_IDLE = 0,
    UI_BOOT_TRANSITION_FADE_OUT,
    UI_BOOT_TRANSITION_WAIT_MAIN_FRAME,
    UI_BOOT_TRANSITION_FADE_IN,
} ui_boot_transition_t;

typedef struct {
    lv_obj_t *wifi;
    lv_obj_t *bluetooth;
    lv_obj_t *battery;
    lv_obj_t *protocol;
    lv_obj_t *clock;
} ui_status_bar_t;

typedef struct {
    lv_obj_t *startup_root;
    lv_obj_t *boot_gif;
    lv_obj_t *main_page;
    lv_obj_t *charging_page;
    lv_obj_t *standby_page;
    lv_obj_t *activation_page;
    lv_obj_t *voice_page;
    ui_status_bar_t main_status;
    ui_status_bar_t charging_status;
    ui_status_bar_t voice_status;
    lv_obj_t *main_time;
    lv_obj_t *main_date;
    lv_obj_t *main_temperature;
    lv_obj_t *main_weather_text;
    lv_obj_t *main_weather_loading;
    lv_obj_t *main_weather_spinner;
    lv_obj_t *main_text_layer;
    lv_obj_t *main_weather_layer;
    lv_obj_t *charge_bolt;
    lv_obj_t *charge_power;
    lv_obj_t *charge_protocol;
    lv_obj_t *charge_soc;
    lv_obj_t *standby_eye_left;
    lv_obj_t *standby_eye_right;
    lv_obj_t *activation_code;
    lv_obj_t *voice_expression;
    lv_obj_t *voice_status_text;
    ui_page_t page;
    bool first_frame_presented;
    int displayed_weather_code;
    int64_t boot_started_us;
    volatile bool boot_sound_window_open;
    volatile bool boot_sound_finished;
    volatile bool boot_transition_complete;
    bool boot_transition_pending;
    ui_boot_transition_t boot_transition;
    int64_t boot_transition_started_us;
    bool main_first_frame_flushed;
    uint8_t main_frame_rows[ST7789_HEIGHT];
    uint16_t main_frame_rows_seen;
    bool main_page_prepared;
    volatile bool main_page_stable;
    int64_t main_page_entered_us;
    int64_t last_activity_us;
    int64_t charge_below_since_us;
    int64_t activation_restart_due_us;
    int64_t standby_expression_started_us;
    int64_t weather_loading_started_us;
    bool weather_loading_timed_out;
    uint32_t standby_expression_phase_us;
    uint32_t voice_pulse_frame;
    uint32_t charge_pulse_frame;
    volatile bool activity_pending;
    bool snapshot_read_unavailable_logged;
    uint8_t text_flush_log_budget;
    uint32_t boot_flush_count;
    int64_t boot_flush_total_us;
    int64_t boot_flush_max_us;
    char logged_date[32];
    char logged_weather[24];
} ui_scene_t;

static TaskHandle_t s_task;
static lv_display_t *s_display;
static lv_color_t s_draw_buffer[ST7789_WIDTH * LUMIDOCK_LVGL_BUFFER_ROWS];
static ui_scene_t s_scene;

bool ui_service_is_main_stable(void)
{
    return s_scene.main_page_stable;
}

bool ui_service_boot_is_active(void)
{
    return s_scene.page == UI_PAGE_BOOT;
}

bool ui_service_boot_sound_window_open(void)
{
    return s_scene.boot_sound_window_open;
}

void ui_service_note_boot_sound_finished(void)
{
    s_scene.boot_sound_finished = true;
}

bool ui_service_boot_transition_complete(void)
{
    return s_scene.boot_transition_complete;
}

static void set_page(ui_page_t page);
static void apply_page_visibility(ui_page_t page);

static const lv_point_precise_t s_bolt_points[] = {
    { 17, 0 }, { 3, 23 }, { 13, 23 }, { 6, 45 }, { 30, 16 }, { 19, 16 }, { 17, 0 },
};

static lv_color_t color(uint16_t rgb565)
{
    return lv_color_hex(((rgb565 & 0xf800U) << 8U) |
                        ((rgb565 & 0x07e0U) << 5U) | ((rgb565 & 0x001fU) << 3U));
}

static void display_flush(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    const int64_t flush_started_us = esp_timer_get_time();
    if (s_scene.text_flush_log_budget > 0U) {
        ESP_LOGI("ui_service", "text flush area: x=%d y=%d w=%d h=%d",
                 (int)area->x1, (int)area->y1,
                 (int)(area->x2 - area->x1 + 1),
                 (int)(area->y2 - area->y1 + 1));
        s_scene.text_flush_log_budget--;
    }
    st7789_draw_bitmap(area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1,
                       (const uint16_t *)px_map);
    if (s_scene.page == UI_PAGE_MAIN &&
        s_scene.boot_transition == UI_BOOT_TRANSITION_WAIT_MAIN_FRAME) {
        const int y1 = area->y1 < 0 ? 0 : area->y1;
        const int y2 = area->y2 >= ST7789_HEIGHT ? ST7789_HEIGHT - 1 : area->y2;
        for (int y = y1; y <= y2; ++y) {
            if (s_scene.main_frame_rows[y] == 0U) {
                s_scene.main_frame_rows[y] = 1U;
                s_scene.main_frame_rows_seen++;
            }
        }
        if (!s_scene.main_first_frame_flushed &&
            s_scene.main_frame_rows_seen >= ST7789_HEIGHT) {
            s_scene.main_first_frame_flushed = true;
            ESP_LOGI("ui_service", "main first frame flushed: rows=%u", (unsigned)s_scene.main_frame_rows_seen);
        }
    }
    if (s_scene.page == UI_PAGE_BOOT) {
        const int64_t flush_us = esp_timer_get_time() - flush_started_us;
        s_scene.boot_flush_count++;
        s_scene.boot_flush_total_us += flush_us;
        if (flush_us > s_scene.boot_flush_max_us) s_scene.boot_flush_max_us = flush_us;
        if (s_scene.boot_flush_count >= 25U) {
            ESP_LOGI("ui_service", "boot GIF flush: avg=%lldus max=%lldus area=%dx%d",
                     (long long)(s_scene.boot_flush_total_us / (int64_t)s_scene.boot_flush_count),
                     (long long)s_scene.boot_flush_max_us,
                     (int)(area->x2 - area->x1 + 1), (int)(area->y2 - area->y1 + 1));
            s_scene.boot_flush_count = 0U;
            s_scene.boot_flush_total_us = 0;
            s_scene.boot_flush_max_us = 0;
        }
    }
    lv_display_flush_ready(display);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t text_color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, text_color, 0);
    return label;
}

static void set_status_icon(lv_obj_t *icon, const lv_image_dsc_t *online, bool is_online)
{
    if (is_online) {
        lv_image_set_src(icon, online);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_status_bar(lv_obj_t *parent, ui_status_bar_t *bar, bool compact_clock)
{
    bar->wifi = lv_image_create(parent);
    bar->bluetooth = lv_image_create(parent);
    bar->battery = lv_image_create(parent);
    lv_obj_add_flag(bar->wifi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bar->bluetooth, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bar->battery, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(bar->wifi, 18, 6);
    lv_obj_set_pos(bar->bluetooth, 46, 6);
    lv_obj_set_pos(bar->battery, 74, 6);
    bar->protocol = make_label(parent, "PD", &lv_font_montserrat_14, color(UI_ACCENT));
    lv_obj_align(bar->protocol, LV_ALIGN_TOP_RIGHT, -14, 8);
    bar->clock = NULL;
    if (compact_clock) {
        bar->clock = make_label(parent, "00:00", &lv_font_montserrat_14, color(UI_INK));
        lv_obj_align(bar->clock, LV_ALIGN_TOP_RIGHT, -52, 8);
    }
}

static void update_status_bar(const ui_status_bar_t *bar, const lumidock_snapshot_t *snapshot, bool compact_clock)
{
    set_status_icon(bar->wifi, &ui_icon_wifi, snapshot->wifi.connected);
    set_status_icon(bar->bluetooth, &ui_icon_bluetooth, snapshot->bluetooth.connected);
    set_status_icon(bar->battery, &ui_icon_battery, snapshot->bms.online);
    lv_label_set_text(bar->protocol, snapshot->charging.valid ? snapshot->charging.name : "PD");
    if (compact_clock && bar->clock != NULL) {
        char clock[8];
        if (snapshot->time_weather.valid) {
            (void)strftime(clock, sizeof(clock), "%H:%M", &snapshot->time_weather.local_time);
            lv_label_set_text(bar->clock, clock);
        } else {
            lv_label_set_text(bar->clock, "00:00");
        }
    }
}

static const lv_image_dsc_t *weather_scene_for_code(int weather_code)
{
    if (weather_code == 45 || weather_code == 48) return &ui_weather_fog_scene;
    if (weather_code >= 51 && weather_code <= 67) return &ui_weather_rain_scene;
    if (weather_code >= 71 && weather_code <= 77) return &ui_weather_snow_scene;
    if (weather_code >= 80 && weather_code <= 82) return &ui_weather_rain_scene;
    if (weather_code >= 85 && weather_code <= 86) return &ui_weather_snow_scene;
    if (weather_code >= 95) return &ui_weather_thunder_scene;
    if (weather_code >= 1 && weather_code <= 3) return &ui_weather_cloudy_scene;
    return &ui_weather_clear_scene;
}

static const char *weather_label_for_code(int weather_code)
{
    if (weather_code == 45 || weather_code == 48) return "雾";
    if ((weather_code >= 51 && weather_code <= 67) || (weather_code >= 80 && weather_code <= 82)) return "雨";
    if ((weather_code >= 71 && weather_code <= 77) || (weather_code >= 85 && weather_code <= 86)) return "雪";
    if (weather_code >= 95) return "雷";
    if (weather_code >= 1 && weather_code <= 3) return "云";
    return "晴";
}

static void layout_main_weather_group(void)
{
    const int weather_width = s_scene.main_weather_text != NULL
                                  ? lv_obj_get_width(s_scene.main_weather_text)
                                  : 0;
    const int weather_x = ST7789_WIDTH - UI_MAIN_RIGHT_MARGIN - weather_width;
    const int temperature_width = lv_obj_get_width(s_scene.main_temperature);
    const int weather_height = s_scene.main_weather_text != NULL
                                   ? lv_obj_get_height(s_scene.main_weather_text)
                                   : 0;
    const int temperature_bottom = lv_obj_get_y(s_scene.main_temperature) + lv_obj_get_height(s_scene.main_temperature);
    int temperature_x = weather_x - UI_MAIN_WEATHER_GAP - temperature_width;
    if (temperature_x < 0) temperature_x = 0;
    if (s_scene.main_weather_text != NULL) {
        lv_obj_set_pos(s_scene.main_weather_text, weather_x,
                       temperature_bottom - weather_height);
    }
    lv_obj_set_x(s_scene.main_temperature, temperature_x);
}

static void refresh_main_text_layer(void)
{
    if (s_scene.main_text_layer == NULL) return;
    lv_obj_update_layout(s_scene.main_page);
    lv_obj_update_layout(s_scene.main_text_layer);
    layout_main_weather_group();
    /* Resolve the style coordinates written above before geometry is logged
     * or the next display refresh is scheduled. */
    lv_obj_update_layout(s_scene.main_text_layer);
    lv_obj_move_foreground(s_scene.main_text_layer);
    lv_obj_invalidate(s_scene.main_text_layer);
}

static void delete_main_date_label(void)
{
    if (s_scene.main_date == NULL) return;
    lv_obj_delete(s_scene.main_date);
    s_scene.main_date = NULL;
}

static void delete_main_weather_label(void)
{
    if (s_scene.main_weather_text == NULL) return;
    lv_obj_delete(s_scene.main_weather_text);
    s_scene.main_weather_text = NULL;
}

static void show_main_weather_loading(void)
{
    lv_image_set_src(s_scene.main_weather_layer, &ui_main_scene);
    lv_obj_invalidate(s_scene.main_weather_layer);
    if (s_scene.main_weather_spinner == NULL) {
        s_scene.main_weather_spinner = lv_spinner_create(s_scene.main_text_layer);
        lv_obj_set_size(s_scene.main_weather_spinner, 24, 24);
        lv_obj_align(s_scene.main_weather_spinner, LV_ALIGN_TOP_MID, 0, 108);
        lv_spinner_set_anim_params(s_scene.main_weather_spinner, 720, 270);
        lv_obj_set_style_arc_width(s_scene.main_weather_spinner, 3, LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_scene.main_weather_spinner, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_scene.main_weather_spinner, color(UI_MUTED), LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_scene.main_weather_spinner, color(UI_INK), LV_PART_INDICATOR);
    } else {
        lv_obj_remove_flag(s_scene.main_weather_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    /* Loading is represented by the spinner alone.  Keep the timeout label
     * hidden in case a later retry re-enters the loading state. */
    if (s_scene.main_weather_loading != NULL) {
        lv_obj_add_flag(s_scene.main_weather_loading, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_main_weather_loading(void)
{
    if (s_scene.main_weather_spinner != NULL) {
        lv_obj_add_flag(s_scene.main_weather_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_scene.main_weather_loading != NULL) {
        lv_obj_add_flag(s_scene.main_weather_loading, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_main_weather_unavailable(void)
{
    if (s_scene.main_weather_spinner != NULL) {
        lv_obj_add_flag(s_scene.main_weather_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_scene.main_weather_loading == NULL) {
        s_scene.main_weather_loading = make_label(s_scene.main_text_layer, "天气暂不可用",
                                                   &ui_cjk_font_14, color(UI_MUTED));
        lv_obj_align(s_scene.main_weather_loading, LV_ALIGN_TOP_MID, 0, 137);
    } else {
        lv_label_set_text(s_scene.main_weather_loading, "天气暂不可用");
        lv_obj_remove_flag(s_scene.main_weather_loading, LV_OBJ_FLAG_HIDDEN);
    }
}

static void log_cjk_glyph(const char *name, uint32_t codepoint)
{
    lv_font_glyph_dsc_t glyph = {0};
    const bool found = lv_font_get_glyph_dsc(&ui_cjk_font_14, &glyph, codepoint, 0);
    ESP_LOGI("ui_service", "CJK glyph %s: found=%d box=%ux%u adv=%d",
             name, found ? 1 : 0, glyph.box_w, glyph.box_h, glyph.adv_w);
}

static uint32_t decode_utf8_codepoint(const char *text)
{
    if (text == NULL || text[0] == '\0') return 0U;
    const uint8_t c0 = (uint8_t)text[0];
    if ((c0 & 0x80U) == 0U) return c0;
    if ((c0 & 0xe0U) == 0xc0U && text[1] != '\0') {
        return ((uint32_t)(c0 & 0x1fU) << 6U) |
               ((uint32_t)((uint8_t)text[1] & 0x3fU));
    }
    if ((c0 & 0xf0U) == 0xe0U && text[1] != '\0' && text[2] != '\0') {
        return ((uint32_t)(c0 & 0x0fU) << 12U) |
               ((uint32_t)((uint8_t)text[1] & 0x3fU) << 6U) |
               ((uint32_t)((uint8_t)text[2] & 0x3fU));
    }
    if ((c0 & 0xf8U) == 0xf0U && text[1] != '\0' && text[2] != '\0' && text[3] != '\0') {
        return ((uint32_t)(c0 & 0x07U) << 18U) |
               ((uint32_t)((uint8_t)text[1] & 0x3fU) << 12U) |
               ((uint32_t)((uint8_t)text[2] & 0x3fU) << 6U) |
               ((uint32_t)((uint8_t)text[3] & 0x3fU));
    }
    return 0U;
}

static void create_main_date_label(const char *text)
{
    s_scene.main_date = make_label(s_scene.main_text_layer, text, &ui_cjk_font_14, color(UI_INK));
    lv_obj_align(s_scene.main_date, LV_ALIGN_TOP_MID, 0, UI_MAIN_DATE_Y);
    lv_obj_move_foreground(s_scene.main_date);
    s_scene.text_flush_log_budget = 8U;
    log_cjk_glyph("star", 0x661fU);
    log_cjk_glyph("period", 0x671fU);
    ESP_LOGI("ui_service", "main date label created: text='%s'", text);
}

static void create_main_weather_label(const char *text)
{
    s_scene.main_weather_text = make_label(s_scene.main_text_layer, text, &ui_cjk_font_14, color(UI_INK));
    lv_obj_move_foreground(s_scene.main_weather_text);
    s_scene.text_flush_log_budget = 8U;
    log_cjk_glyph("weather", decode_utf8_codepoint(text));
    ESP_LOGI("ui_service", "main weather label created: text='%s'", text);
}

static void format_chinese_date(char *out, size_t out_size, const struct tm *value)
{
    static const char *const weekdays[] = { "日", "一", "二", "三", "四", "五", "六" };
    const int weekday = (value->tm_wday >= 0 && value->tm_wday <= 6) ? value->tm_wday : 0;
    (void)snprintf(out, out_size, "星期%s %02d月%02d日", weekdays[weekday],
                   value->tm_mon + 1, value->tm_mday);
}

static void update_boot_page(int64_t now_us)
{
    (void)now_us;
}

static void create_startup_page(lv_obj_t *root)
{
    s_scene.startup_root = lv_obj_create(root);
    lv_obj_remove_style_all(s_scene.startup_root);
    lv_obj_set_size(s_scene.startup_root, ST7789_WIDTH, ST7789_HEIGHT);
    lv_obj_set_style_bg_color(s_scene.startup_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scene.startup_root, LV_OPA_COVER, 0);
    s_scene.boot_gif = lv_gif_create(s_scene.startup_root);
    lv_gif_set_color_format(s_scene.boot_gif, LV_COLOR_FORMAT_RGB565);
    lv_gif_set_src(s_scene.boot_gif, &ui_boot_animation);
    lv_obj_center(s_scene.boot_gif);
    ESP_LOGI("ui_service", "boot GIF ready: 144x80 figure-eight, min=10s max=15s");
}

static void create_main_page(lv_obj_t *root)
{
    s_scene.main_page = lv_obj_create(root);
    lv_obj_remove_style_all(s_scene.main_page);
    lv_obj_set_size(s_scene.main_page, ST7789_WIDTH, ST7789_HEIGHT);
    lv_obj_set_style_bg_color(s_scene.main_page, color(UI_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(s_scene.main_page, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_scene.main_page, LV_OBJ_FLAG_HIDDEN);
    s_scene.main_weather_layer = lv_image_create(s_scene.main_page);
    lv_image_set_src(s_scene.main_weather_layer, &ui_main_scene);
    lv_obj_set_pos(s_scene.main_weather_layer, 0, 0);
    create_status_bar(s_scene.main_page, &s_scene.main_status, false);
    s_scene.main_time = make_label(s_scene.main_page, "00:00", &ui_time_font_64, color(UI_INK));
    lv_obj_align(s_scene.main_time, LV_ALIGN_TOP_MID, 0, 29);
    s_scene.main_date = NULL;
    s_scene.main_weather_text = NULL;
    s_scene.main_weather_loading = NULL;
    s_scene.main_weather_spinner = NULL;
    s_scene.main_temperature = make_label(s_scene.main_page, "0°", &lv_font_montserrat_32, color(UI_INK));
    lv_obj_set_pos(s_scene.main_temperature, 0, 110);
    lv_obj_move_foreground(s_scene.main_temperature);
    s_scene.main_text_layer = lv_obj_create(s_scene.main_page);
    lv_obj_remove_style_all(s_scene.main_text_layer);
    lv_obj_set_size(s_scene.main_text_layer, ST7789_WIDTH, ST7789_HEIGHT);
    lv_obj_set_pos(s_scene.main_text_layer, 0, 0);
    lv_obj_set_style_bg_opa(s_scene.main_text_layer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_scene.main_text_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_scene.main_text_layer);
    layout_main_weather_group();
}

static void create_charging_page(lv_obj_t *root)
{
    s_scene.charging_page = lv_obj_create(root);
    lv_obj_remove_style_all(s_scene.charging_page);
    lv_obj_set_size(s_scene.charging_page, ST7789_WIDTH, ST7789_HEIGHT);
    lv_obj_set_style_bg_color(s_scene.charging_page, color(UI_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(s_scene.charging_page, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_scene.charging_page, LV_OBJ_FLAG_HIDDEN);
    create_status_bar(s_scene.charging_page, &s_scene.charging_status, true);
    s_scene.charge_bolt = lv_line_create(s_scene.charging_page);
    lv_line_set_points(s_scene.charge_bolt, s_bolt_points, sizeof(s_bolt_points) / sizeof(s_bolt_points[0]));
    lv_obj_set_pos(s_scene.charge_bolt, 92, 55);
    lv_obj_set_style_line_width(s_scene.charge_bolt, 3, 0);
    lv_obj_set_style_line_color(s_scene.charge_bolt, color(UI_ACCENT), 0);
    lv_obj_set_style_line_rounded(s_scene.charge_bolt, true, 0);
    s_scene.charge_power = make_label(s_scene.charging_page, "--.- W", &lv_font_montserrat_48, color(UI_INK));
    lv_obj_align(s_scene.charge_power, LV_ALIGN_TOP_MID, 18, 50);
    s_scene.charge_protocol = make_label(s_scene.charging_page, "PD", &lv_font_montserrat_20, color(UI_ACCENT));
    lv_obj_align(s_scene.charge_protocol, LV_ALIGN_TOP_MID, 0, 111);
    s_scene.charge_soc = make_label(s_scene.charging_page, "--%", &lv_font_montserrat_16, color(UI_MUTED));
    lv_obj_align(s_scene.charge_soc, LV_ALIGN_TOP_MID, 0, 140);
}

static void style_standby_eye(lv_obj_t *eye)
{
    lv_obj_remove_style_all(eye);
    lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
}

static void create_standby_page(lv_obj_t *root)
{
    s_scene.standby_page = lv_obj_create(root);
    lv_obj_remove_style_all(s_scene.standby_page);
    lv_obj_set_size(s_scene.standby_page, ST7789_WIDTH, ST7789_HEIGHT);
    lv_obj_set_style_bg_color(s_scene.standby_page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scene.standby_page, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_scene.standby_page, LV_OBJ_FLAG_HIDDEN);

    s_scene.standby_eye_left = lv_obj_create(s_scene.standby_page);
    s_scene.standby_eye_right = lv_obj_create(s_scene.standby_page);
    style_standby_eye(s_scene.standby_eye_left);
    style_standby_eye(s_scene.standby_eye_right);
    lv_obj_set_size(s_scene.standby_eye_left, UI_STANDBY_EYE_CLOSED_WIDTH,
                    UI_STANDBY_EYE_CLOSED_HEIGHT);
    lv_obj_set_size(s_scene.standby_eye_right, UI_STANDBY_EYE_CLOSED_WIDTH,
                    UI_STANDBY_EYE_CLOSED_HEIGHT);
    lv_obj_set_pos(s_scene.standby_eye_left,
                   UI_STANDBY_EYE_LEFT_CENTER - UI_STANDBY_EYE_CLOSED_WIDTH / 2,
                   UI_STANDBY_EYE_CENTER_Y - UI_STANDBY_EYE_CLOSED_HEIGHT / 2);
    lv_obj_set_pos(s_scene.standby_eye_right,
                   UI_STANDBY_EYE_RIGHT_CENTER - UI_STANDBY_EYE_CLOSED_WIDTH / 2,
                   UI_STANDBY_EYE_CENTER_Y - UI_STANDBY_EYE_CLOSED_HEIGHT / 2);
}

static void create_activation_page(lv_obj_t *root)
{
    s_scene.activation_page = lv_obj_create(root);
    lv_obj_remove_style_all(s_scene.activation_page);
    lv_obj_set_size(s_scene.activation_page, ST7789_WIDTH, ST7789_HEIGHT);
    lv_obj_set_style_bg_color(s_scene.activation_page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scene.activation_page, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_scene.activation_page, LV_OBJ_FLAG_HIDDEN);

    s_scene.activation_code = make_label(s_scene.activation_page, "------",
                                         &lv_font_montserrat_32, color(UI_ACCENT));
    lv_obj_center(s_scene.activation_code);
}

static void create_voice_page(lv_obj_t *root)
{
    s_scene.voice_page = lv_obj_create(root);
    lv_obj_remove_style_all(s_scene.voice_page);
    lv_obj_set_size(s_scene.voice_page, ST7789_WIDTH, ST7789_HEIGHT);
    lv_obj_set_style_bg_color(s_scene.voice_page, color(UI_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(s_scene.voice_page, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_scene.voice_page, LV_OBJ_FLAG_HIDDEN);
    create_status_bar(s_scene.voice_page, &s_scene.voice_status, true);
    s_scene.voice_expression = lv_image_create(s_scene.voice_page);
    lv_image_set_src(s_scene.voice_expression, &ui_voice_connecting);
    lv_obj_align(s_scene.voice_expression, LV_ALIGN_TOP_MID, 0, 52);
    s_scene.voice_status_text = make_label(s_scene.voice_page, "CONNECTING",
                                            &lv_font_montserrat_16, color(UI_MUTED));
    lv_obj_align(s_scene.voice_status_text, LV_ALIGN_TOP_MID, 0, 116);
}

static void update_boot_state(int64_t now_us, const lumidock_snapshot_t *snapshot)
{
    const int64_t elapsed_us = now_us - s_scene.boot_started_us;
    const bool data_ready = snapshot != NULL && snapshot->time_weather.valid;
    if (!s_scene.boot_sound_window_open &&
        ((data_ready && elapsed_us >= UI_BOOT_MIN_DURATION_US - UI_BOOT_SOUND_LEAD_US) ||
         elapsed_us >= UI_BOOT_MAX_DURATION_US - UI_BOOT_SOUND_LEAD_US)) {
        s_scene.boot_sound_window_open = true;
        ESP_LOGI("ui_service", "boot sound window opened: %lld ms before boot transition",
                 (long long)(UI_BOOT_SOUND_LEAD_US / 1000LL));
    }
    if ((elapsed_us >= UI_BOOT_MAX_DURATION_US && s_scene.boot_sound_finished) ||
        (elapsed_us >= UI_BOOT_MIN_DURATION_US && data_ready &&
         s_scene.boot_sound_window_open && s_scene.boot_sound_finished)) {
        s_scene.boot_transition_pending = true;
    }
}

static void apply_page_visibility(ui_page_t page)
{
    lv_obj_add_flag(s_scene.startup_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_scene.main_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_scene.charging_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_scene.standby_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_scene.activation_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_scene.voice_page, LV_OBJ_FLAG_HIDDEN);
    if (page == UI_PAGE_BOOT) lv_obj_remove_flag(s_scene.startup_root, LV_OBJ_FLAG_HIDDEN);
    else if (page == UI_PAGE_MAIN) lv_obj_remove_flag(s_scene.main_page, LV_OBJ_FLAG_HIDDEN);
    else if (page == UI_PAGE_CHARGING) lv_obj_remove_flag(s_scene.charging_page, LV_OBJ_FLAG_HIDDEN);
    else if (page == UI_PAGE_STANDBY) lv_obj_remove_flag(s_scene.standby_page, LV_OBJ_FLAG_HIDDEN);
    else if (page == UI_PAGE_ACTIVATION) lv_obj_remove_flag(s_scene.activation_page, LV_OBJ_FLAG_HIDDEN);
    else if (page == UI_PAGE_VOICE) lv_obj_remove_flag(s_scene.voice_page, LV_OBJ_FLAG_HIDDEN);
}

static void start_main_page_from_boot(int64_t now_us)
{
    memset(s_scene.main_frame_rows, 0, sizeof(s_scene.main_frame_rows));
    s_scene.main_frame_rows_seen = 0U;
    s_scene.main_first_frame_flushed = false;
    apply_page_visibility(UI_PAGE_MAIN);
    s_scene.page = UI_PAGE_MAIN;
    refresh_main_text_layer();
    lv_obj_update_layout(s_scene.main_page);
    lv_obj_invalidate(s_scene.main_page);
    s_scene.last_activity_us = now_us;
    s_scene.main_page_entered_us = now_us;
    s_scene.main_page_stable = false;
    s_scene.boot_transition = UI_BOOT_TRANSITION_WAIT_MAIN_FRAME;
    ESP_LOGI("ui_service", "boot transition fade out complete; main frame pending");
}

static void update_boot_transition(int64_t now_us)
{
    if (s_scene.boot_transition == UI_BOOT_TRANSITION_IDLE) return;
    const int64_t elapsed_us = now_us - s_scene.boot_transition_started_us;
    if (s_scene.boot_transition == UI_BOOT_TRANSITION_FADE_OUT) {
        if (elapsed_us >= UI_BOOT_FADE_OUT_US) start_main_page_from_boot(now_us);
    } else if (s_scene.boot_transition == UI_BOOT_TRANSITION_WAIT_MAIN_FRAME) {
        if (s_scene.main_first_frame_flushed) {
            s_scene.boot_transition = UI_BOOT_TRANSITION_FADE_IN;
            s_scene.boot_transition_started_us = now_us;
            st7789_fade_backlight_percent(100U, (uint32_t)(UI_BOOT_FADE_IN_US / 1000LL));
            ESP_LOGI("ui_service", "main first frame flushed; boot transition fade in");
        }
    } else if (s_scene.boot_transition == UI_BOOT_TRANSITION_FADE_IN) {
        if (elapsed_us >= UI_BOOT_FADE_IN_US) {
            s_scene.boot_transition = UI_BOOT_TRANSITION_IDLE;
            s_scene.boot_transition_complete = true;
            ESP_LOGI("ui_service", "UI boot complete -> main");
            ESP_LOGI("ui_service", "boot transition fade in complete");
        }
    }
}

static bool charging_active(const lumidock_snapshot_t *snapshot)
{
    return snapshot->bms.online && snapshot->bms.parsed_valid && snapshot->bms.current_ma > UI_CHARGE_ENTER_MA;
}

static float output_power_w(const charging_snapshot_t *charging)
{
    if (!charging->valid || charging->output_voltage_mv == 0U) return -1.0f;
    uint32_t current_ma = charging->port_c_on ? charging->type_c_current_ma : charging->type_a_current_ma;
    if (current_ma == 0U) current_ma = charging->type_c_current_ma != 0U ? charging->type_c_current_ma : charging->type_a_current_ma;
    if (current_ma == 0U) return -1.0f;
    return ((float)charging->output_voltage_mv * (float)current_ma) / 1000000.0f;
}

static void update_main_page(const lumidock_snapshot_t *snapshot)
{
    char text[32];
    char date_text[32];
    bool text_layout_dirty = false;
    update_status_bar(&s_scene.main_status, snapshot, false);
    const bool weather_valid = snapshot->time_weather.weather_text[0] != '\0';
    if (!snapshot->time_weather.valid) {
        const int64_t now_us = esp_timer_get_time();
        if (s_scene.weather_loading_started_us == 0) {
            s_scene.weather_loading_started_us = now_us;
        }
        lv_label_set_text(s_scene.main_time, "00:00");
        delete_main_date_label();
        lv_obj_add_flag(s_scene.main_temperature, LV_OBJ_FLAG_HIDDEN);
        delete_main_weather_label();
        if (!s_scene.weather_loading_timed_out &&
            now_us - s_scene.weather_loading_started_us >= UI_WEATHER_LOADING_TIMEOUT_US) {
            s_scene.weather_loading_timed_out = true;
            ESP_LOGW("ui_service", "weather loading timeout after 10s");
        }
        if (s_scene.weather_loading_timed_out) show_main_weather_unavailable();
        else show_main_weather_loading();
        s_scene.displayed_weather_code = -1;
        refresh_main_text_layer();
        return;
    }
    (void)strftime(text, sizeof(text), "%H:%M", &snapshot->time_weather.local_time);
    lv_label_set_text(s_scene.main_time, text);
    format_chinese_date(date_text, sizeof(date_text), &snapshot->time_weather.local_time);
    if (s_scene.main_date == NULL) {
        create_main_date_label(date_text);
        text_layout_dirty = true;
    } else if (strcmp(lv_label_get_text(s_scene.main_date), date_text) != 0) {
        lv_label_set_text(s_scene.main_date, date_text);
        lv_obj_align(s_scene.main_date, LV_ALIGN_TOP_MID, 0, UI_MAIN_DATE_Y);
        text_layout_dirty = true;
    }
    if (!weather_valid) {
        const int64_t now_us = esp_timer_get_time();
        if (s_scene.weather_loading_started_us == 0) s_scene.weather_loading_started_us = now_us;
        if (!s_scene.weather_loading_timed_out &&
            now_us - s_scene.weather_loading_started_us >= UI_WEATHER_LOADING_TIMEOUT_US) {
            s_scene.weather_loading_timed_out = true;
            ESP_LOGW("ui_service", "weather loading timeout after 10s");
        }
        lv_obj_add_flag(s_scene.main_temperature, LV_OBJ_FLAG_HIDDEN);
        delete_main_weather_label();
        if (s_scene.weather_loading_timed_out) show_main_weather_unavailable();
        else show_main_weather_loading();
        s_scene.displayed_weather_code = -1;
        refresh_main_text_layer();
        return;
    }
    s_scene.weather_loading_started_us = 0;
    s_scene.weather_loading_timed_out = false;
    hide_main_weather_loading();
    lv_obj_remove_flag(s_scene.main_temperature, LV_OBJ_FLAG_HIDDEN);
    (void)snprintf(text, sizeof(text), "%.0f°", (double)snapshot->time_weather.temperature_c);
    lv_label_set_text(s_scene.main_temperature, text);
    {
        if (s_scene.main_weather_text == NULL) {
            create_main_weather_label(weather_label_for_code(snapshot->time_weather.weather_code));
            text_layout_dirty = true;
        }
        if (s_scene.displayed_weather_code != snapshot->time_weather.weather_code) {
            lv_image_set_src(s_scene.main_weather_layer, weather_scene_for_code(snapshot->time_weather.weather_code));
            lv_obj_invalidate(s_scene.main_weather_layer);
            lv_label_set_text(s_scene.main_weather_text, weather_label_for_code(snapshot->time_weather.weather_code));
            s_scene.displayed_weather_code = snapshot->time_weather.weather_code;
            text_layout_dirty = true;
            ESP_LOGI("ui_service", "weather scene updated: code=%d", snapshot->time_weather.weather_code);
        }
    }
    if (text_layout_dirty) lv_obj_update_layout(s_scene.main_page);
    refresh_main_text_layer();
    if (strcmp(s_scene.logged_date, date_text) != 0 ||
        strcmp(s_scene.logged_weather, snapshot->time_weather.weather_text) != 0) {
        ESP_LOGI("ui_service", "main data: date='%s' weather='%s' temp=%.1f wifi=%d internet=%d",
                 date_text,
                 snapshot->time_weather.weather_text,
                 (double)snapshot->time_weather.temperature_c,
                 snapshot->wifi.connected ? 1 : 0,
                 snapshot->wifi.internet_online ? 1 : 0);
        ESP_LOGI("ui_service", "main text geometry: date=(%d,%d,%d,%d,hidden=%d) weather=(%d,%d,%d,%d,hidden=%d) parent_hidden=%d",
                 s_scene.main_date != NULL ? (int)lv_obj_get_x(s_scene.main_date) : 0,
                 s_scene.main_date != NULL ? (int)lv_obj_get_y(s_scene.main_date) : 0,
                 s_scene.main_date != NULL ? (int)lv_obj_get_width(s_scene.main_date) : 0,
                 s_scene.main_date != NULL ? (int)lv_obj_get_height(s_scene.main_date) : 0,
                 s_scene.main_date != NULL && lv_obj_has_flag(s_scene.main_date, LV_OBJ_FLAG_HIDDEN) ? 1 : 0,
                 s_scene.main_weather_text != NULL ? (int)lv_obj_get_x(s_scene.main_weather_text) : 0,
                 s_scene.main_weather_text != NULL ? (int)lv_obj_get_y(s_scene.main_weather_text) : 0,
                 s_scene.main_weather_text != NULL ? (int)lv_obj_get_width(s_scene.main_weather_text) : 0,
                 s_scene.main_weather_text != NULL ? (int)lv_obj_get_height(s_scene.main_weather_text) : 0,
                 s_scene.main_weather_text != NULL && lv_obj_has_flag(s_scene.main_weather_text, LV_OBJ_FLAG_HIDDEN) ? 1 : 0,
                 lv_obj_has_flag(s_scene.main_page, LV_OBJ_FLAG_HIDDEN) ? 1 : 0);
        (void)strlcpy(s_scene.logged_date, date_text, sizeof(s_scene.logged_date));
        (void)strlcpy(s_scene.logged_weather, snapshot->time_weather.weather_text,
                      sizeof(s_scene.logged_weather));
    }
}

static void update_charging_page(const lumidock_snapshot_t *snapshot)
{
    char text[32];
    update_status_bar(&s_scene.charging_status, snapshot, true);
    const float power = output_power_w(&snapshot->charging);
    if (power < 0.0f) lv_label_set_text(s_scene.charge_power, "--.- W");
    else {
        (void)snprintf(text, sizeof(text), "%.1f W", (double)power);
        lv_label_set_text(s_scene.charge_power, text);
    }
    lv_label_set_text(s_scene.charge_protocol, snapshot->charging.valid ? snapshot->charging.name : "PD");
    if (snapshot->bms.online) {
        (void)snprintf(text, sizeof(text), "%u%%", snapshot->bms.soc_percent);
        lv_label_set_text(s_scene.charge_soc, text);
    } else lv_label_set_text(s_scene.charge_soc, "--%");
    const uint32_t pulse = (s_scene.charge_pulse_frame++ / 5U) % 2U;
    lv_obj_set_style_opa(s_scene.charge_bolt, pulse == 0U ? LV_OPA_COVER : LV_OPA_70, 0);
}

static uint32_t standby_smoothstep(uint32_t progress)
{
    const uint64_t p = progress;
    return (uint32_t)((p * p * (3000U - 2U * progress)) / 1000000ULL);
}

static void update_standby_expression(int64_t now_us)
{
    if (s_scene.standby_expression_started_us == 0) {
        s_scene.standby_expression_started_us = now_us;
    }

    const int64_t elapsed_us = now_us - s_scene.standby_expression_started_us;
    const uint32_t phase_us = (uint32_t)(elapsed_us >= 0 ?
        (elapsed_us % UI_STANDBY_EXPRESSION_PERIOD_US) : 0);
    s_scene.standby_expression_phase_us = phase_us;

    uint32_t open = 0;
    const uint32_t open_start_us = 960000U;
    const uint32_t open_end_us = 1472000U;
    const uint32_t close_start_us = 2240000U;
    const uint32_t close_end_us = 2752000U;
    if (phase_us >= open_start_us && phase_us < open_end_us) {
        open = standby_smoothstep((phase_us - open_start_us) * 1000U /
                                  (open_end_us - open_start_us));
    } else if (phase_us >= open_end_us && phase_us < close_start_us) {
        open = 1000U;
    } else if (phase_us >= close_start_us && phase_us < close_end_us) {
        const uint32_t progress = standby_smoothstep((phase_us - close_start_us) * 1000U /
                                                      (close_end_us - close_start_us));
        open = 1000U - progress;
    }

    const lv_coord_t eye_width = (lv_coord_t)(UI_STANDBY_EYE_CLOSED_WIDTH +
        ((UI_STANDBY_EYE_OPEN_WIDTH - UI_STANDBY_EYE_CLOSED_WIDTH) * open) / 1000U);
    const lv_coord_t eye_height = (lv_coord_t)(UI_STANDBY_EYE_CLOSED_HEIGHT +
        ((UI_STANDBY_EYE_OPEN_HEIGHT - UI_STANDBY_EYE_CLOSED_HEIGHT) * open) / 1000U);
    const lv_coord_t eye_y = UI_STANDBY_EYE_CENTER_Y - eye_height / 2;
    lv_obj_set_size(s_scene.standby_eye_left, eye_width, eye_height);
    lv_obj_set_size(s_scene.standby_eye_right, eye_width, eye_height);
    lv_obj_set_pos(s_scene.standby_eye_left,
                   UI_STANDBY_EYE_LEFT_CENTER - eye_width / 2, eye_y);
    lv_obj_set_pos(s_scene.standby_eye_right,
                   UI_STANDBY_EYE_RIGHT_CENTER - eye_width / 2, eye_y);
}

static void update_standby_page(const lumidock_snapshot_t *snapshot, int64_t now_us)
{
    (void)snapshot;
    update_standby_expression(now_us);
}

static void update_activation_page(const lumidock_snapshot_t *snapshot)
{
    lv_label_set_text(s_scene.activation_code, snapshot->voice_activation.activation_code);
}

static const lv_image_dsc_t *voice_expression_for_state(voice_ui_state_t state)
{
    switch (state) {
    case VOICE_UI_LISTENING: return &ui_voice_listening;
    case VOICE_UI_THINKING: return &ui_voice_thinking;
    case VOICE_UI_SPEAKING: return &ui_voice_speaking;
    case VOICE_UI_ERROR: return &ui_voice_error;
    case VOICE_UI_CONNECTING:
    case VOICE_UI_IDLE:
    default: return &ui_voice_connecting;
    }
}

static const char *voice_text_for_state(voice_ui_state_t state)
{
    switch (state) {
    case VOICE_UI_LISTENING: return "LISTENING";
    case VOICE_UI_THINKING: return "THINKING";
    case VOICE_UI_SPEAKING: return "SPEAKING";
    case VOICE_UI_ERROR: return "RETRYING";
    case VOICE_UI_CONNECTING: return "CONNECTING";
    case VOICE_UI_IDLE:
    default: return "";
    }
}

static void update_voice_page(const lumidock_snapshot_t *snapshot)
{
    update_status_bar(&s_scene.voice_status, snapshot, true);
    const voice_ui_state_t state = snapshot->voice_session.state;
    lv_image_set_src(s_scene.voice_expression, voice_expression_for_state(state));
    lv_label_set_text(s_scene.voice_status_text, voice_text_for_state(state));
    if ((s_scene.voice_pulse_frame++ % 6U) == 0U &&
        (state == VOICE_UI_LISTENING || state == VOICE_UI_SPEAKING)) {
        const bool dim = ((s_scene.voice_pulse_frame / 6U) % 2U) != 0U;
        lv_obj_set_style_opa(s_scene.voice_expression, dim ? LV_OPA_70 : LV_OPA_COVER, 0);
    } else if (state != VOICE_UI_LISTENING && state != VOICE_UI_SPEAKING) {
        lv_obj_set_style_opa(s_scene.voice_expression, LV_OPA_COVER, 0);
    }
}

static void set_page(ui_page_t page)
{
    if (s_scene.page == page) return;
    apply_page_visibility(page);
    s_scene.page = page;
    s_scene.standby_expression_phase_us = 0;
    s_scene.standby_expression_started_us = 0;
    if (page == UI_PAGE_STANDBY) st7789_set_backlight_percent(25U);
    else st7789_set_backlight_percent(100U);
    if (page == UI_PAGE_MAIN) {
        refresh_main_text_layer();
        lv_obj_invalidate(s_scene.main_page);
        s_scene.last_activity_us = esp_timer_get_time();
        s_scene.main_page_entered_us = s_scene.last_activity_us;
        s_scene.main_page_stable = false;
    } else {
        s_scene.main_page_stable = false;
    }
}

void ui_service_note_activity(void)
{
    s_scene.activity_pending = true;
}

esp_err_t ui_service_request_exit_standby(void)
{
    if (s_task == NULL) return ESP_ERR_INVALID_STATE;
    ui_service_note_activity();
    ESP_LOGI("ui_service", "voice requested standby exit");
    return ESP_OK;
}

static void update_page_state(const lumidock_snapshot_t *snapshot, int64_t now_us)
{
    if (s_scene.activity_pending) {
        s_scene.activity_pending = false;
        s_scene.last_activity_us = now_us;
        if (s_scene.page == UI_PAGE_STANDBY) set_page(UI_PAGE_MAIN);
    }
    if (s_scene.page == UI_PAGE_BOOT) return;
    if (snapshot->voice_activation.restart_required) {
        if (s_scene.activation_restart_due_us == 0) {
            s_scene.activation_restart_due_us = now_us + UI_ACTIVATION_RESTART_DELAY_US;
            set_page(UI_PAGE_ACTIVATION);
            ESP_LOGI("ui_service", "XiaoZhi activation confirmed; restarting device");
        } else if (now_us >= s_scene.activation_restart_due_us) {
            esp_restart();
        }
        return;
    }
    if (snapshot->voice_activation.initialized && !snapshot->voice_activation.activated &&
        snapshot->voice_activation.activation_code[0] != '\0') {
        if (s_scene.page != UI_PAGE_ACTIVATION) set_page(UI_PAGE_ACTIVATION);
        return;
    }
    if (s_scene.page == UI_PAGE_ACTIVATION) set_page(UI_PAGE_MAIN);
    if (snapshot->voice_session.initialized && snapshot->voice_session.active &&
        snapshot->voice_session.state != VOICE_UI_IDLE) {
        if (s_scene.page != UI_PAGE_VOICE) set_page(UI_PAGE_VOICE);
        return;
    }
    if (s_scene.page == UI_PAGE_VOICE) {
        set_page(charging_active(snapshot) ? UI_PAGE_CHARGING : UI_PAGE_MAIN);
    }
    if (charging_active(snapshot)) {
        s_scene.charge_below_since_us = 0;
        if (s_scene.page != UI_PAGE_CHARGING) set_page(UI_PAGE_CHARGING);
        return;
    }
    if (s_scene.page == UI_PAGE_CHARGING) {
        if (snapshot->bms.current_ma <= UI_CHARGE_EXIT_MA) {
            if (s_scene.charge_below_since_us == 0) s_scene.charge_below_since_us = now_us;
            if (now_us - s_scene.charge_below_since_us >= UI_CHARGE_EXIT_HOLD_US) set_page(UI_PAGE_MAIN);
        } else s_scene.charge_below_since_us = 0;
        return;
    }
    if (s_scene.page == UI_PAGE_MAIN && now_us - s_scene.last_activity_us >= UI_STANDBY_TIMEOUT_US) {
        set_page(UI_PAGE_STANDBY);
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    (void)watchdog_service_subscribe_current_task();
    for (;;) {
        watchdog_service_feed();
        const int64_t now_us = esp_timer_get_time();
        const lumidock_snapshot_t *snapshot = NULL;
        if (mid_snapshot_read_acquire(&snapshot)) {
            s_scene.snapshot_read_unavailable_logged = false;
            if (s_scene.page == UI_PAGE_BOOT) {
                update_boot_page(now_us);
                update_boot_state(now_us, snapshot);
                if (s_scene.boot_transition_pending &&
                    s_scene.boot_transition == UI_BOOT_TRANSITION_IDLE) {
                    s_scene.boot_transition_pending = false;
                    s_scene.boot_transition = UI_BOOT_TRANSITION_FADE_OUT;
                    s_scene.boot_transition_started_us = now_us;
                    st7789_fade_backlight_percent(0U, (uint32_t)(UI_BOOT_FADE_OUT_US / 1000LL));
                    ESP_LOGI("ui_service", "boot transition fade out");
                }
            } else {
                if (s_scene.boot_transition == UI_BOOT_TRANSITION_IDLE) {
                    update_page_state(snapshot, now_us);
                }
                if (s_scene.page == UI_PAGE_MAIN) update_main_page(snapshot);
                else if (s_scene.page == UI_PAGE_CHARGING) update_charging_page(snapshot);
                else if (s_scene.page == UI_PAGE_STANDBY) update_standby_page(snapshot, now_us);
                else if (s_scene.page == UI_PAGE_ACTIVATION) update_activation_page(snapshot);
                else if (s_scene.page == UI_PAGE_VOICE) update_voice_page(snapshot);
            }
            if (s_scene.page == UI_PAGE_MAIN && !s_scene.main_page_stable &&
                s_scene.main_page_entered_us > 0 &&
                now_us - s_scene.main_page_entered_us >= UI_MAIN_STABLE_DELAY_US) {
                s_scene.main_page_stable = true;
                ESP_LOGI("ui_service", "main page stable; deferred services may start");
            }
            mid_snapshot_read_release();
        } else if (!s_scene.snapshot_read_unavailable_logged) {
            ESP_LOGW("ui_service", "snapshot read unavailable; skipping UI data frame");
            s_scene.snapshot_read_unavailable_logged = true;
        }
        if (s_scene.page == UI_PAGE_BOOT) {
            update_boot_page(now_us);
            update_boot_state(now_us, NULL);
        }
        lv_timer_handler();
        update_boot_transition(esp_timer_get_time());
        if (!s_scene.first_frame_presented) {
            st7789_set_backlight(true);
            s_scene.first_frame_presented = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        lv_tick_inc(20);
    }
}

esp_err_t ui_service_init(void)
{
    memset(&s_scene, 0, sizeof(s_scene));
    s_scene.page = UI_PAGE_BOOT;
    s_scene.displayed_weather_code = -1;
    s_scene.last_activity_us = esp_timer_get_time();
    const esp_err_t err = st7789_init();
    if (err != ESP_OK) return err;
    lv_init();
    s_display = lv_display_create(ST7789_WIDTH, ST7789_HEIGHT);
    if (s_display == NULL) return ESP_ERR_NO_MEM;
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, s_draw_buffer, NULL, sizeof(s_draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, display_flush);
    lv_obj_t *root = lv_screen_active();
    create_startup_page(root);
    create_main_page(root);
    create_charging_page(root);
    create_standby_page(root);
    create_activation_page(root);
    create_voice_page(root);
    s_scene.boot_started_us = esp_timer_get_time();
    if (xTaskCreatePinnedToCore(ui_task, "ui_service", 8192, NULL, 2, &s_task, 1) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI("ui_service", "UI boot entered");
    ESP_LOGI("ui_service", "LVGL startup, main, charging and standby pages enabled");
    return ESP_OK;
}
