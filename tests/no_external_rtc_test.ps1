$ErrorActionPreference = 'Stop'

$cmake = Get-Content -Raw 'main/CMakeLists.txt'
$sourceFiles = @(
    'main/Dri/ds3231.c',
    'main/Dri/ds3231.h',
    'main/APP/app_rtc.c',
    'main/APP/app_rtc.h'
)

foreach ($file in $sourceFiles) {
    if (Test-Path $file) {
        throw "external RTC file still exists: $file"
    }
}

if ($cmake -match '(?i)ds3231|app_rtc') {
    throw 'main/CMakeLists.txt still includes external RTC sources'
}

$source = Get-ChildItem -Path 'main' -Recurse -File -Include *.c,*.h |
    Get-Content -Raw
if ($source -match '(?i)ds3231|app_rtc_set_from_system|LUMIDOCK_DISABLE_EXTERNAL_RTC|WEATHER_TEMP_SOURCE_RTC|set_rtc_temperature|RTC 温度') {
    throw 'main source still references removed external RTC implementation'
}

Write-Output 'no_external_rtc_test: PASS'

