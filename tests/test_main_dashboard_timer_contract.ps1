$ErrorActionPreference = 'Stop'

$snapshotHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\mid_snapshot.h"
$timerHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\timer_service.h"
$timerSource = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\timer_service.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"
$assets = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_assets.c"
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"
$cjkHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_cjk_font.h"
$timerSvg = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\assets\ui\timer_icon.svg"
$alarmSvg = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\assets\ui\alarm_icon.svg"

@(
    @{ Content = $snapshotHeader; Pattern = 'timer_snapshot_t'; Message = 'The UI snapshot must expose timer state.' },
    @{ Content = $snapshotHeader; Pattern = 'mid_snapshot_set_timer'; Message = 'Timer state must have a snapshot publisher.' },
    @{ Content = $timerHeader; Pattern = 'timer_service_get_snapshot'; Message = 'Timer service must expose a read-only UI snapshot.' },
    @{ Content = $timerSource; Pattern = 'alarm_active'; Message = 'Timer snapshot must expose the next active alarm.' },
    @{ Content = $voice; Pattern = 'mid_snapshot_set_timer'; Message = 'Voice timer poll must publish timer state.' },
    @{ Content = $ui; Pattern = 'snapshot->timer'; Message = 'Main UI must consume timer state only through the snapshot.' },
    @{ Content = $ui; Pattern = 'timer_display_active'; Message = 'Countdown and stopwatch must take display priority.' },
    @{ Content = $ui; Pattern = 'UI_MAIN_TIMER_PREVIEW_ENABLED 0'; Message = 'Main page must not force-show timer preview values.' },
    @{ Content = $ui; Pattern = 'main_weather_icon'; Message = 'Main page must render the selected weather icon.' },
    @{ Content = $ui; Pattern = 'UI_MAIN_HEADER_WIDTH'; Message = 'Header block must have fixed dimensions.' },
    @{ Content = $ui; Pattern = 'UI_MAIN_HEADER_WIDTH 310'; Message = 'Header width must include the requested 10px increase.' },
    @{ Content = $ui; Pattern = 'UI_MAIN_HEADER_Y 31'; Message = 'Date and weekday header position must remain unchanged.' },
    @{ Content = $ui; Pattern = 'main_header_group'; Message = 'Time, date, and weekday must share a header container.' },
    @{ Content = $ui; Pattern = 'lv_obj_set_style_pad_column\(s_scene\.main_header_group, 15, 0\)'; Message = 'Header labels must use the requested 15px gap.' },
    @{ Content = $ui; Pattern = '&ui_time_font_64'; Message = 'Main time must use the native 64px time font.' },
    @{ Content = $ui; Pattern = 'lv_obj_set_style_translate_y\(s_scene\.main_time, 14, 0\)'; Message = 'Time label must keep its approved 14px vertical offset.' },
    @{ Content = $ui; Pattern = 'lv_obj_set_style_translate_x\(s_scene\.main_date, 8, 0\)'; Message = 'Date label must shift right by 8px.' },
    @{ Content = $ui; Pattern = 'lv_obj_set_style_translate_x\(s_scene\.main_weekday, 8, 0\)'; Message = 'Weekday label must shift right by 8px.' },
    @{ Content = $ui; Pattern = 'main_weekday'; Message = 'Date and weekday must have dedicated labels.' },
    @{ Content = $ui; Pattern = 'main_calendar_group'; Message = 'Date and weekday must share a vertical calendar group.' },
    @{ Content = $ui; Pattern = '&ui_cjk_font_34_black'; Message = 'Date and weekday must use the 34px bold (Hei) CJK font.' },
    @{ Content = $cjkHeader; Pattern = 'ui_cjk_font_34'; Message = 'The 34px CJK font must be declared to the UI.' },
    @{ Content = $ui; Pattern = 'main_timer_group'; Message = 'Timer icon and time must share a fixed-size container.' },
    @{ Content = $ui; Pattern = 'UI_MAIN_TIMER_GROUP_WIDTH 136'; Message = 'Timer group must have fixed width.' },
    @{ Content = $ui; Pattern = 'UI_MAIN_TIMER_GROUP_Y 114'; Message = 'Timer group must move down by 11px.' },
    @{ Content = $ui; Pattern = 'main_timer_icon'; Message = 'Countdown and alarm display must have a dedicated icon object.' },
    @{ Content = $assets; Pattern = 'ui_weather_icon_sunny'; Message = 'Sunny Iconfont asset must be embedded.' },
    @{ Content = $assets; Pattern = 'LV_COLOR_FORMAT_RGB565A8'; Message = 'Weather icons must preserve transparent backgrounds.' },
    @{ Content = $assets; Pattern = 'ui_weather_icon_sunny, ui_weather_icon_sunny_data, 72, 72'; Message = 'Weather icons must use the approved 72px size.' },
    @{ Content = $assets; Pattern = 'ui_icon_timer, ui_icon_timer_data, 34, 34'; Message = 'Timer Iconfont asset must use the approved 34px size.' },
    @{ Content = $assets; Pattern = 'ui_icon_alarm, ui_icon_alarm_data, 34, 34'; Message = 'Alarm Iconfont asset must use the approved 34px size.' },
    @{ Content = $cmake; Pattern = 'assets/ui/weather_icon_sunny\.rgb565a8'; Message = 'Sunny transparent Iconfont asset must be embedded by CMake.' },
    @{ Content = $cmake; Pattern = 'assets/ui/weather_icon_snow\.rgb565a8'; Message = 'Snow transparent Iconfont asset must be embedded by CMake.' },
    @{ Content = $cmake; Pattern = 'assets/ui/timer_icon\.rgb565a8'; Message = 'Timer Iconfont asset must be embedded by CMake.' },
    @{ Content = $cmake; Pattern = 'assets/ui/alarm_icon\.rgb565a8'; Message = 'Alarm Iconfont asset must be embedded by CMake.' }
) | ForEach-Object { if ($_.Content -notmatch $_.Pattern) { throw $_.Message } }

if ($timerSvg -notmatch 'width="34" height="34"' -or $alarmSvg -notmatch 'width="34" height="34"') {
    throw 'Timer and alarm SVG roots must be normalized to 34px before rasterization.'
}

if ($ui -match 'update_status_bar\(&s_scene\.main_status, snapshot, false\)') {
    throw 'The main dashboard must not restore Bluetooth or battery icons through the shared status update.'
}

if ($ui -notmatch 'weather_scene_for_code\(snapshot->time_weather.weather_code\)') {
    throw 'The main background must keep switching by weather code.'
}

foreach ($scene in @('weather_clear_scene.svg', 'weather_cloudy_scene.svg', 'weather_fog_scene.svg', 'weather_rain_scene.svg', 'weather_snow_scene.svg', 'weather_thunder_scene.svg')) {
    $sceneContent = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\assets\ui\$scene"
    if ($sceneContent -match 'data-weather-icon') {
        throw "$scene must not retain a background weather icon."
    }
}

Write-Output 'Main dashboard timer contract passed.'
