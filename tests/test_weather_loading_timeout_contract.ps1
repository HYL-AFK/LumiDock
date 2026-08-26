$ErrorActionPreference = 'Stop'
$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"
$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"

if ($ui -notmatch 'UI_WEATHER_LOADING_TIMEOUT_US\s+\(10000000LL\)') {
    throw 'Weather loading must have a 10 second UI timeout.'
}
if ($ui -notmatch '天气暂不可用') {
    throw 'Weather timeout must expose an unavailable state instead of spinning forever.'
}
if ($ui -notmatch 'weather_loading_timed_out') {
    throw 'UI must remember that the initial weather load timed out.'
}
if ($ui -match 'make_label\([^;]*"LOADING"') {
    throw 'The loading state must use animation only, without a LOADING text label.'
}
if ($manager -notmatch 'VOICE_INITIAL_NETWORK_SETTLE_TIMEOUT_MS\s+15000U') {
    throw 'Voice startup timeout must remain independent from the weather UI timeout.'
}
Write-Output 'Weather loading timeout contract passed.'
