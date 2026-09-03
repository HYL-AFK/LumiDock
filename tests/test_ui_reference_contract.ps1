$ErrorActionPreference = 'Stop'
$ui = Get-Content -Raw -Encoding UTF8 -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"
$weather = Get-Content -Raw -Encoding UTF8 -LiteralPath "$PSScriptRoot\..\main\MID\network\weather_service.c"
$defaults = Get-Content -Raw -Encoding UTF8 -LiteralPath "$PSScriptRoot\..\sdkconfig.defaults"
$assets = Get-Content -Raw -Encoding UTF8 -LiteralPath "$PSScriptRoot\..\main\APP\ui_assets.c"
$wifi = Get-Content -Raw -Encoding UTF8 -LiteralPath "$PSScriptRoot\..\main\Dri\wifi_manager.c"

if ($defaults -match 'CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_14_CJK=y') {
    throw 'The incomplete built-in Source Han CJK font must be disabled after the project subset font is added.'
}
if ($ui -match 'lv_font_source_han_sans_sc_14_cjk') {
    throw 'The incomplete built-in CJK font must not be used by any UI label.'
}
if ($ui -notmatch '#include "ui_cjk_font\.h"' -or
    $ui -notmatch '&ui_cjk_font_14') {
    throw 'All Chinese UI labels must use the project CJK subset font.'
}
$cjkHeader = Get-Content -Raw -Encoding UTF8 -LiteralPath "$PSScriptRoot\..\main\APP\ui_cjk_font.h"
$cjkSource = Get-Content -Raw -Encoding UTF8 -LiteralPath "$PSScriptRoot\..\main\APP\ui_cjk_font.c"
$cmake = Get-Content -Raw -Encoding UTF8 -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"
if ($cjkHeader -notmatch 'LV_FONT_DECLARE\(ui_cjk_font_14\)' -or
    $cjkSource -notmatch 'ui_cjk_font_14' -or
    $cmake -notmatch 'APP/ui_cjk_font\.c') {
    throw 'The project must provide the complete UI Chinese subset font.'
}

# Every Han character used by a user-visible UI string must have a real glyph
# in the generated subset.  Checking only a hand-maintained sample lets a new
# status label regress to LVGL's missing-glyph box unnoticed.
$uiStringLiterals = [regex]::Matches($ui, '"(?:\\.|[^"\\])*"') | ForEach-Object { $_.Value }
$uiHanCharacters = @($uiStringLiterals.ToCharArray() |
    Where-Object { ([int][char]$_ -ge 0x3400) -and ([int][char]$_ -le 0x9FFF) } |
    Sort-Object -Unique)
foreach ($character in $uiHanCharacters) {
    $code = ('{0:X4}' -f [int][char]$character)
    if ($cjkSource -notmatch "U\+$code") {
        throw "The UI CJK subset is missing glyph U+$code ($character)."
    }
}
if ($ui -notmatch 's_scene\.main_time\s*=\s*make_label\([^;]*ui_time_font_64' -or
    $ui -notmatch '#include "ui_time_font\.h"') {
    throw 'The time label must use the dedicated native 64 px numeric font for sharp rendering.'
}
if ($ui -match 'lv_obj_set_style_transform_scale\(s_scene\.time') {
    throw 'The time label must not use bitmap scaling because it blurs and shifts the glyphs.'
}
if ($ui -match '"%a  %m/%d"') {
    throw 'The reference UI must not render an English abbreviated date.'
}
if ($ui -notmatch '\u661F\u671F') {
    throw 'The reference UI must render Chinese weekday text.'
}
if ($ui -notmatch 'weather_label_for_code' -or
    $weather -notmatch 'weather_text\(int code\)') {
    throw 'Weather conditions must retain one shared compact mapping for the UI.'
}
if ($assets -notmatch 'ui_weather_clear_scene') {
    throw 'The reference weather scene must remain Flash-backed.'
}
if ($wifi -match 'esp_wifi_set_config\(WIFI_IF_STA, sta_cfg\)') {
    throw 'Wi-Fi configuration must pass a writable local copy to the ESP-IDF API.'
}

Write-Output 'UI reference contract checks passed.'

