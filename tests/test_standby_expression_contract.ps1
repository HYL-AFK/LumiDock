$ErrorActionPreference = 'Stop'

$ui = Get-Content -Raw (Join-Path $PSScriptRoot '..\main\APP\ui_service.c')

if ($ui -notmatch 'UI_STANDBY_EXPRESSION_PERIOD_US\s+\(3200000LL\)') {
    throw 'Standby expression must use the approved 3.2 second loop.'
}

if ($ui -match 'static void create_standby_page\([\s\S]*?static void create_activation_page' -and
    $Matches[0] -match 'standby_time|ui_time_font_64') {
    throw 'Standby must not create or update a time label.'
}

$standby_update = [regex]::Match($ui, 'static void update_standby_page\([\s\S]*?static void update_activation_page').Value
if ($standby_update -match 'standby_time|strftime') {
    throw 'Standby must not update a time label.'
}

foreach ($field in @('standby_eye_left', 'standby_eye_right')) {
    if ($ui -notmatch "lv_obj_t \*${field}") {
        throw "Standby expression field is missing: $field"
    }
}

foreach ($constant in @('UI_STANDBY_EYE_OPEN_WIDTH 76', 'UI_STANDBY_EYE_OPEN_HEIGHT 68',
                        'UI_STANDBY_EYE_CLOSED_WIDTH 64', 'UI_STANDBY_EYE_CLOSED_HEIGHT 12',
                        'UI_STANDBY_EYE_LEFT_CENTER 105', 'UI_STANDBY_EYE_RIGHT_CENTER 215',
                        'UI_STANDBY_EYE_CENTER_Y 85')) {
    if ($ui -notmatch [regex]::Escape($constant)) {
        throw "Standby geometry constant is missing: $constant"
    }
}

if ($ui -notmatch 'static void update_standby_expression\(' -or
    $ui -notmatch 'update_standby_expression\(now_us\)') {
    throw 'Standby expression must be updated by the UI task.'
}

if ($ui -notmatch 'standby_expression_phase_us\s*=\s*0') {
    throw 'Entering standby must reset the expression phase.'
}

if ($ui -match 'standby.*gif|lv_gif_create', [Text.RegularExpressions.RegexOptions]::IgnoreCase) {
    throw 'Standby expression must use local LVGL geometry, not a GIF decoder.'
}

Write-Output 'standby expression contract passed'
