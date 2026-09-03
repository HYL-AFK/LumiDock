$ErrorActionPreference = 'Stop'
$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"
$font = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_time_font.c"
$snapshotHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\mid_snapshot.h"
$snapshotSource = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\mid_snapshot.c"
$driver = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\st7789_driver.c"
$header = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\st7789_driver.h"
$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"
$defaults = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig.defaults"
$sdkconfig = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig"
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"
$assetsHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_assets.h"
$assetsSource = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_assets.c"
$bootGif = Join-Path $PSScriptRoot '..\main\assets\ui\boot_animation.gif'

if ($font -notmatch '\.range_start = 48' -or
    $font -notmatch '\.range_length = 11' -or
    $font -notmatch 'LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY') {
    throw 'The native time font must map the contiguous 0-9 and colon range.'
}
if ($font -match 'range_length = 0') {
    throw 'The native time font must not use a zero-length character map.'
}
if ($ui -match 'ESPARK' -or $ui -match 'boot_ring') {
    throw 'The startup page must not retain the removed brand text or ring animation.'
}
if ($ui -notmatch 'boot_gif' -or
    $ui -notmatch 'lv_gif_create\(s_scene\.startup_root\)' -or
    $ui -notmatch 'lv_gif_set_color_format\(s_scene\.boot_gif, LV_COLOR_FORMAT_RGB565\)' -or
    $ui -notmatch 'lv_gif_set_src\(s_scene\.boot_gif, &ui_boot_animation\)' -or
    $ui -notmatch 'lv_obj_center\(s_scene\.boot_gif\)' -or
    $ui -match 'boot_progress_frame' -or
    $ui -match 'boot_ellipsis' -or
    $ui -match 'make_label\(s_scene\.startup_root, "Starting",') {
    throw 'The boot page must play the embedded figure-eight GIF without the temporary text or progress-bar animation.'
}
if ($defaults -notmatch 'CONFIG_LV_USE_CLIB_MALLOC=y' -or
    $defaults -match 'CONFIG_LV_USE_BUILTIN_MALLOC=y' -or
    $sdkconfig -notmatch 'CONFIG_LV_USE_CLIB_MALLOC=y' -or
    $sdkconfig -match 'CONFIG_LV_USE_BUILTIN_MALLOC=y') {
    throw 'The full-screen GIF framebuffer must use ESP-IDF malloc and PSRAM, not the fixed LVGL pool.'
}
if ($ui -match 'boot_shadow' -or
    $ui -match 'trail_x' -or
    $ui -match 'arc_progress' -or
    $ui -match 'arc_height') {
    throw 'The startup page must not retain the fixed-shadow or one-way parabolic animation.'
}
if ($ui -match 'boot_dot\[UI_BOOT_DOT_COUNT\]' -or
    $ui -match 'boot_keyframe_t' -or
    $ui -match 's_boot_keyframes' -or
    $ui -match 'boot_interpolate') {
    throw 'The startup page must not retain the hand-written dot/keyframe animation.'
}
if (!(Test-Path -LiteralPath $bootGif)) {
    throw 'The embedded boot GIF resource is missing.'
}
$gifBytes = [System.IO.File]::ReadAllBytes($bootGif)
if ($gifBytes.Length -gt 16384 -or
    [Text.Encoding]::ASCII.GetString($gifBytes, 0, 6) -notmatch '^GIF8[79]a' -or
    [BitConverter]::ToUInt16($gifBytes, 6) -ne 144 -or
    [BitConverter]::ToUInt16($gifBytes, 8) -ne 80) {
    throw 'The boot GIF must be a compressed 144x80 moving-dot region under 16 KiB.'
}
$gifFrames = 0
$gifDelays = [System.Collections.Generic.List[int]]::new()
$gifIndex = 13
if (($gifBytes[10] -band 0x80) -ne 0) {
    $gifIndex += 3 * (1 -shl (($gifBytes[10] -band 0x07) + 1))
}
while ($gifIndex -lt $gifBytes.Length) {
    $block = $gifBytes[$gifIndex]
    $gifIndex++
    if ($block -eq 0x3b) { break }
    if ($block -eq 0x21) {
        $label = $gifBytes[$gifIndex]
        $gifIndex++
        if ($label -eq 0xf9) {
            $size = $gifBytes[$gifIndex]
            $gifIndex++
            if ($size -ne 4) { throw 'The boot GIF graphics-control block is malformed.' }
            $gifDelays.Add([BitConverter]::ToUInt16($gifBytes, $gifIndex + 1))
            $gifIndex += $size + 1
            continue
        }
        while ($gifIndex -lt $gifBytes.Length) {
            $size = $gifBytes[$gifIndex]
            $gifIndex++
            if ($size -eq 0) { break }
            $gifIndex += $size
        }
        continue
    }
    if ($block -eq 0x2c) {
        $gifFrames++
        $packed = $gifBytes[$gifIndex + 8]
        $gifIndex += 9
        if (($packed -band 0x80) -ne 0) {
            $gifIndex += 3 * (1 -shl (($packed -band 0x07) + 1))
        }
        $gifIndex++
        while ($gifIndex -lt $gifBytes.Length) {
            $size = $gifBytes[$gifIndex]
            $gifIndex++
            if ($size -eq 0) { break }
            $gifIndex += $size
        }
        continue
    }
    throw 'The boot GIF contains an unexpected block.'
}
if ($gifFrames -lt 24 -or $gifDelays.Count -ne $gifFrames -or
    (@($gifDelays | Where-Object { $_ -ne 4 }).Count -ne 0)) {
    throw 'The boot GIF must retain individual frames at a fixed 40 ms cadence; merged or irregular frames cause loop stutter.'
}
if ($ui -notmatch 'UI_BOOT_MIN_DURATION_US \(10000000LL\)' -or
    $ui -notmatch 'UI_BOOT_MAX_DURATION_US \(15000000LL\)') {
    throw 'The figure-eight boot animation must wait at least 10 seconds and no more than 15 seconds.'
}
if ($ui -notmatch 'boot GIF flush: avg=' -or
    $ui -notmatch 'boot_flush_total_us' -or
    $ui -notmatch 'boot_flush_max_us') {
    throw 'The startup GIF must report low-frequency frame flush timing for hardware performance verification.'
}
if ($ui -match 'boot_triangle_position' -or
    $ui -match 'boot_figure_eight_position' -or
    $ui -match 'UI_BOOT_TRIANGLE_SEGMENT_FRAMES' -or
    $ui -match 'dot_phase\s*=' -or
    $ui -match 'UI_BOOT_CYCLE_FRAMES / UI_BOOT_DOT_COUNT') {
    throw 'The startup page must not retain a hand-written trajectory or shared phase model.'
}
if ($ui -match 'LV_OPA_35') {
    throw 'The startup animation must not use an opacity macro unsupported by this LVGL version.'
}
if ($ui -notmatch 'UI_BOOT_MIN_DURATION_US' -or
    $ui -notmatch 'UI_BOOT_MAX_DURATION_US' -or
    $ui -notmatch 'boot_started_us' -or
    $ui -notmatch 'UI boot complete -> network decision') {
    throw 'The startup page must use a monotonic window and log its transition to the network decision.'
}
if ($ui -notmatch 'UI_BOOT_FADE_OUT_US \(80000LL\)' -or
    $ui -notmatch 'UI_BOOT_FADE_IN_US \(160000LL\)' -or
    $ui -notmatch 'UI_BOOT_TRANSITION_WAIT_NETWORK_FRAME' -or
    $ui -notmatch 'network first frame flushed' -or
    $ui -notmatch 'boot transition fade in complete' -or
    $ui -notmatch 'st7789_fade_backlight_percent\(0U' -or
    $ui -notmatch 'st7789_fade_backlight_percent\(100U') {
    throw 'Boot-to-main transition must hold a black field until the complete main frame is flushed, then fade in.'
}
if ($ui -match 'set_page\(UI_PAGE_MAIN\);\s*ESP_LOGI\("ui_service", "UI boot complete') {
    throw 'Boot page must not reveal the main page through the ordinary page switch before the black-field transition.'
}
if ($ui -match 'create_partition_layer' -or $ui -match 'time_zone') {
    throw 'The main page must not render a time partition rectangle.'
}
if ($ui -notmatch 'ui_time_font_64' -or
    $ui -notmatch 'weather_label_for_code' -or
    $ui -notmatch 'layout_main_weather_group') {
    throw 'The main page must use the fixed time font and dynamically lay out the compact weather label.'
}
if ($ui -notmatch 'lv_obj_set_style_translate_y\(s_scene\.main_time, 14, 0\)') {
    throw 'The main time must keep its 14px vertical offset within the header flex row.'
}
if ($ui -notmatch 'lv_obj_set_pos\(s_scene\.main_weather_layer, 0, 0\)') {
    throw 'The weather background must cover the full screen behind the status bar.'
}
$clearWeather = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\assets\ui\weather_clear_scene.svg"
$rainWeather = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\assets\ui\weather_rain_scene.svg"
if ($clearWeather -match 'circle cx="82" cy="39"') {
    throw 'The clear-weather scene must not embed the sun illustration; it lives in the iconfont layer.'
}
if ($rainWeather -match 'translate\(-78 90\)') {
    throw 'The rain-weather scene must not embed the old translated illustration group.'
}
if ($ui -notmatch 'UI_PAGE_CHARGING' -or
    $ui -notmatch 'UI_PAGE_STANDBY' -or
    $ui -notmatch 'UI_STANDBY_TIMEOUT_US \(264000000LL\)') {
    throw 'Charging and 4.4-minute standby pages must be part of the UI state machine.'
}
if ($ui -notmatch 'UI_CHARGE_ENTER_MA 500' -or
    $ui -notmatch 'UI_CHARGE_EXIT_MA 300' -or
    $ui -notmatch 'output_power_w') {
    throw 'Charging entry, exit hysteresis, and output power calculation are required.'
}
if ($snapshotHeader -notmatch 'mid_snapshot_read_acquire' -or
    $snapshotSource -notmatch '\*out = &s_snapshot' -or
    $snapshotSource -notmatch 'mid_snapshot_read_release') {
    throw 'The UI must be able to use a protected read-only snapshot pointer.'
}
if ($ui -notmatch 'lumidock_snapshot_t\s+snapshot_copy' -or
    $ui -notmatch 'mid_snapshot_get\(&snapshot_copy\)') {
    throw 'The UI task must copy snapshots before rendering.'
}
if ($ui -match 'snapshot read unavailable; skipping UI data frame') {
    throw 'The UI must not skip an entire data frame when snapshot writers are active.'
}
if ($ui -notmatch 'if \(s_scene\.page == UI_PAGE_BOOT\)' -or
    $ui -notmatch 'update_boot_page\(now_us\)') {
    throw 'Incoming snapshots must not replace the startup page before its complete animation duration.'
}
if ($manager.IndexOf('watchdog_service_init()') -gt $manager.IndexOf('ui_service_init()')) {
    throw 'The watchdog must be initialized before the UI task can subscribe in the normal application profile.'
}
if ($ui -match 'lv_obj_invalidate\(lv_screen_active\(\)\)' -or
    $ui -match 'lv_refr_now\(s_display\)') {
    throw 'Page transitions must not force a synchronous full-screen refresh.'
}
if ($ui -notmatch 's_scene\.main_time = make_label\(s_scene\.main_header_group, "00:00",' -or
    $ui -notmatch 's_scene\.main_temperature = make_label\(s_scene\.main_page, "0°",') {
    throw 'Offline main-page UI placeholders must use 00:00 and 0 degrees instead of dashes.'
}
if ($ui -notmatch 'delete_main_date_label\(\)' -or
    $ui -notmatch 'delete_main_weather_label\(\)') {
    throw 'Offline date and weather labels must be deleted instead of retaining zero-width controls.'
}
if ($header -notmatch 'st7789_set_backlight_percent' -or
    $driver -notmatch 'ledc_timer_config' -or
    $ui -notmatch 'st7789_set_backlight_percent\(25U\)') {
    throw 'Standby must use PWM backlight dimming at 25 percent.'
}

Write-Output 'UI layering, page flow, pointer read, and PWM checks passed.'

