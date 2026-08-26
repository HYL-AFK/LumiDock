$assets = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_assets.c"
$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"

if ($ui -notmatch 'UI_ACCENT\s*=\s*0x354d') {
    throw 'UI accent must be expressed as a 16-bit RGB565 value before it is passed to color().'
}

if ($assets -match '#define UI_IMAGE\(name, data, width, height\)') {
    throw 'UI_IMAGE must not use data as a macro parameter because it replaces the lv_image_dsc_t .data field.'
}
if ($assets -notmatch '#define UI_IMAGE\(name, source, width, height\)') {
    throw 'UI_IMAGE must use a non-conflicting source parameter.'
}
if ($assets -notmatch '\.data = \(source\)') {
    throw 'UI_IMAGE must initialize the lv_image_dsc_t data field from the source parameter.'
}
if ($assets -match '_binary_assets_ui_') {
    throw 'EMBED_FILES symbols are derived from the resource basename, not its source directory path.'
}
@(
    '_binary_main_scene_rgb565_start',
    '_binary_wifi_rgb565_start',
    '_binary_bluetooth_rgb565_start',
    '_binary_battery_rgb565_start',
    '_binary_weather_clear_scene_i8_start',
    '_binary_weather_cloudy_scene_i8_start',
    '_binary_weather_rain_scene_i8_start',
    '_binary_weather_thunder_scene_i8_start'
) | ForEach-Object {
    if ($assets -notmatch [regex]::Escape($_)) {
        throw "UI asset is missing the ESP-IDF linker symbol $_."
    }
}
if ($assets -match '_offline_data') {
    throw 'Offline icon assets must not remain embedded when disconnected icons are hidden.'
}

Write-Output 'UI asset macro checks passed.'

