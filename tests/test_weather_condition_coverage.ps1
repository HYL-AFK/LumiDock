$weather = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\network\weather_service.c"
$source = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_weather.c"
$rain = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\assets\ui\weather_rain_scene.svg"
$snow = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\assets\ui\weather_snow_scene.svg"
$thunder = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\assets\ui\weather_thunder_scene.svg"
$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"
$assets = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_assets.c"
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"

if ($weather -notmatch 'code >= 51' -or
    $weather -notmatch 'code >= 71' -or
    $weather -notmatch 'code == 95' -or
    $weather -notmatch 'code >= 96') {
    throw 'Weather service must retain rain, snow, and thunder WMO mappings.'
}
if ($weather -match 'code == 61|code == 65|code == 75') {
    throw 'Weather service must not preserve detailed precipitation text branches.'
}
if ($source -notmatch 'weather_code == 45' -or
    $source -notmatch 'weather_code >= 96' -or
    $source -notmatch 'weather_code >= 80') {
    throw 'The WMO weather-code branches do not cover fog, hail thunderstorms, and showers.'
}
@(
    @{ Name = 'rain'; Content = $rain; Color = '#3F8F98'; Token = 'M9 12c.53.14' },
    @{ Name = 'snow'; Content = $snow; Color = '#4F91AF'; Token = 'M6 14a1 1 0 0 1 1 1' },
    @{ Name = 'thunder'; Content = $thunder; Color = '#687E86'; Token = 'M6 16a5 5 0 0 1-5-5' }
) | ForEach-Object {
    if ($_.Content -notmatch [regex]::Escape($_.Color) -or
        $_.Content -notmatch [regex]::Escape($_.Token) -or
        $_.Content -notmatch 'radialGradient') {
        throw "The $($_.Name) scene must use the approved icon and a soft radial background."
    }
}

if ($ui -notmatch 'weather_code >= 51' -or
    $ui -notmatch 'weather_code >= 71' -or
    $ui -notmatch 'weather_code >= 95' -or
    $ui -match 'return -70;') {
    throw 'Rain, snow, and thunder artwork must use the full-width scene without a negative layer offset.'
}

@('weather_clear_scene', 'weather_cloudy_scene', 'weather_fog_scene',
  'weather_rain_scene', 'weather_snow_scene', 'weather_thunder_scene') | ForEach-Object {
    if ($assets -notmatch [regex]::Escape("ui_$_")) {
        throw "The UI asset table is missing the $_ runtime scene."
    }
    if ($cmake -notmatch [regex]::Escape("$_.i8")) {
        throw "The CMake embed list is missing the $_ resource."
    }
}
if ($ui -match 'strstr\(snapshot->time_weather.weather_text') {
    throw 'Weather artwork must be selected from the numeric WMO code, not localized display text.'
}
if ($ui -notmatch 'weather_code') {
    throw 'The UI must receive a numeric weather code for six-way scene selection.'
}

Write-Output 'Weather condition coverage checks passed.'

