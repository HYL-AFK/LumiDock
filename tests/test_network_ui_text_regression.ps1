$ErrorActionPreference = 'Stop'
$wifi = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\network\wifi_service.c"
$time = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\network\time_service.h"
$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"
$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"

if ($time -notmatch 'time_service_notify_internet_online') {
    throw 'Network-online must have a dedicated notification for location/weather release.'
}
if ($wifi -notmatch 'time_service_notify_internet_online\(\)') {
    throw 'wifi_service must release location/weather only after internet_online is true.'
}
if ($wifi -match 'weather_service_notify_network\(\)') {
    throw 'wifi_service must not duplicate weather requests outside the location coordinator.'
}
$appTime = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_time.c"
if ($appTime -notmatch 'cached_weather_released' -or
    $appTime -notmatch '!cached_weather_released \|\| changed') {
    throw 'Cached coordinates must start one weather request and suppress an unchanged-location duplicate.'
}
if ($ui -notmatch 'lv_obj_update_layout\(s_scene\.main_page\)') {
    throw 'Main text labels must complete LVGL layout before dynamic positioning.'
}
if ($ui -notmatch 'main_date = NULL' -or
    $ui -notmatch 'main_weather_text = NULL') {
    throw 'Date and weather labels must be created lazily from valid data.'
}
if ($ui -notmatch 'main date labels created' -or
    $ui -notmatch 'main weather label created') {
    throw 'Lazy label creation must be observable in the runtime log.'
}
if ($ui -notmatch 'boot_gif' -or
    $ui -notmatch 'lv_gif_create\(s_scene\.startup_root\)' -or
    $ui -notmatch 'lv_gif_set_src\(s_scene\.boot_gif, &ui_boot_animation\)' -or
    $ui -match 'boot_progress_frame' -or
    $ui -match 'lv_bar_create') {
    throw 'Boot animation timing must use the embedded figure-eight GIF without the temporary progress-bar objects.'
}
if ($ui -notmatch 'snapshot->time_weather.valid;') {
    throw 'Boot completion must wait for time only, not weather.'
}
if ($ui -match 'lv_image_set_src\(scene, &ui_main_scene\)') {
    throw 'The covered full-screen white scene must not be drawn beneath the weather layer.'
}
if ($ui -notmatch 'main_page_prepared' -or
    $ui -notmatch 'boot_transition_pending') {
    throw 'Main page data must be prepared before the boot page becomes hidden.'
}
if ($ui -notmatch 'lv_font_get_glyph_dsc' -or
    $ui -notmatch 'text flush area') {
    throw 'Date and weather diagnostics must log font glyphs and their actual display flush areas.'
}
if ($ui -match 'text flush area:.*\n\s+area->x1') {
    throw 'LVGL coordinate values passed to ESP_LOGI must be explicitly cast to int.'
}
if ($manager -notmatch '#define LUMIDOCK_NETWORK_UI_TEST_MODE 1' -or
    $manager -notmatch '#define LUMIDOCK_VOICE_TEST_MODE 1' -or
    $manager -notmatch 'LUMIDOCK_AUDIO_TEST_ENABLED') {
    throw 'Voice testing must be independently enabled while network UI bench mode remains active.'
}

Write-Output 'Network/UI text regression checks passed.'
