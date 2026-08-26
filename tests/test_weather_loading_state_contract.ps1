$ErrorActionPreference = 'Stop'

$ui = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\main\APP\ui_service.c')

@(
    @{ Pattern = 'main_weather_loading'; Message = 'The main scene requires a dedicated weather loading indicator.' },
    @{ Pattern = 'main_weather_spinner'; Message = 'Invalid weather snapshots require a native LVGL spinner.' },
    @{ Pattern = 'lv_spinner_create'; Message = 'Weather loading must use the LVGL spinner widget.' },
    @{ Pattern = 'lv_spinner_set_anim_params'; Message = 'Weather loading spinner must have an explicit animation cadence.' },
    @{ Pattern = 'show_main_weather_loading'; Message = 'Invalid weather snapshots must select the loading state.' },
    @{ Pattern = 'hide_main_weather_loading'; Message = 'A valid weather snapshot must remove the loading state.' },
    @{ Pattern = 'lv_label_set_text\(s_scene\.main_weather_loading'; Message = 'Loading feedback must update without a weather asset.' },
    @{ Pattern = 'lv_image_set_src\(s_scene\.main_weather_layer, &ui_main_scene\)'; Message = 'Loading state must use the white main scene instead of clear weather artwork.' }
) | ForEach-Object {
    if ($ui -notmatch $_.Pattern) { throw $_.Message }
}

$invalidBranch = [regex]::Match($ui, 'if \(!snapshot->time_weather\.valid\) \{.*?\n    \}', 'Singleline').Value
if ($invalidBranch -notmatch 'show_main_weather_loading\(\);') {
    throw 'An invalid weather snapshot must show the loading indicator.'
}
if ($invalidBranch -match 'ui_weather_clear_scene|weather_label_for_code') {
    throw 'An invalid weather snapshot must not render the clear-weather fallback.'
}

Write-Output 'Weather loading state contract passed.'
