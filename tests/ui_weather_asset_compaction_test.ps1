$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$cmake = Get-Content (Join-Path $root 'main/CMakeLists.txt') -Raw -Encoding utf8
$assets = Get-Content (Join-Path $root 'main/APP/ui_assets.c') -Raw -Encoding utf8
$ui = Get-Content (Join-Path $root 'main/APP/ui_service.c') -Raw -Encoding utf8
$weatherService = Get-Content (Join-Path $root 'main/MID/network/weather_service.c') -Raw -Encoding utf8
$uiDir = Join-Path $root 'main/assets/ui'
$sceneNames = @('clear', 'cloudy', 'fog', 'rain', 'snow', 'thunder')

if ($cmake -match 'weather_text_') {
    throw 'Weather text bitmaps must not be embedded.'
}
if ($cmake -match 'weather_.*_scene\.rgb565') {
    throw 'Weather scenes must no longer embed raw RGB565 files.'
}
if ($assets -notmatch 'LV_COLOR_FORMAT_I8') {
    throw 'Weather scenes must use the LVGL I8 indexed color format.'
}
if ($assets -match 'ui_weather_text_') {
    throw 'Weather text image descriptors must be removed.'
}
if ($ui -notmatch 'weather_label_for_code' -or $ui -match 'weather_text_for_code') {
    throw 'The UI must render a compact dynamic weather label instead of a weather text image.'
}
foreach ($codepoint in @(0x6674, 0x4E91, 0x96FE, 0x96E8, 0x96EA, 0x96F7)) {
    $label = [string][char]$codepoint
    if ($ui -notmatch [regex]::Escape("return `"$label`";")) {
        throw 'Weather labels must use the six supported one-character summaries.'
    }
}
if ($ui -match ([string][char]0x6717)) {
    throw 'Weather labels must not use unsupported CJK glyphs.'
}
if ($ui -notmatch 'temperature_bottom - weather_height') {
    throw 'Weather label bottom must align dynamically with the temperature bottom.'
}
if ($weatherService -match '毛毛雨|强降雨|阵雨|暴雪|雷雨冰雹') {
    throw 'The shared weather summary must use the six simplified conditions.'
}

foreach ($name in $sceneNames) {
    $asset = Join-Path $uiDir "weather_${name}_scene.i8"
    if (-not (Test-Path -LiteralPath $asset)) {
        throw "Missing indexed weather asset: $asset"
    }
    if ((Get-Item -LiteralPath $asset).Length -ne 55424) {
        throw "Indexed weather asset must be 55424 bytes for a 320x170 scene: $asset"
    }
    if ($cmake -notmatch [regex]::Escape("weather_${name}_scene.i8")) {
        throw "CMake must embed indexed weather asset weather_${name}_scene.i8"
    }
}

if ($assets -notmatch 'ui_weather_clear_scene, ui_weather_clear_scene_data, 320, 170') {
    throw 'Weather scene descriptors must use the 320x170 canvas.'
}
if ($ui -notmatch 'lv_obj_set_pos\(s_scene\.main_weather_layer, 0, 0\)') {
    throw 'The weather layer must cover the full screen behind the status bar.'
}
if ($ui -match 'weather_scene_x_for_code') {
    throw 'Weather scenes must not use per-condition negative layer offsets.'
}
if ($ui -notmatch 'UI_MAIN_RIGHT_MARGIN 13') {
    throw 'The weather information group must use a 13px right margin.'
}

Write-Output 'Weather assets use compact I8 scenes and dynamic labels.'

