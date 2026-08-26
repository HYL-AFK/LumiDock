$ErrorActionPreference = 'Stop'

$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"

$audioTaskStart = $manager.IndexOf('static void audio_startup_task(void *arg)')
$gateStart = $manager.IndexOf('static void voice_activation_gate_task(void *arg)')
$managerStart = $manager.IndexOf('esp_err_t app_manager_start(void)')
$wifiInit = $manager.IndexOf('wifi_service_init()', $managerStart)
$audioCreate = $manager.IndexOf('xTaskCreatePinnedToCore(audio_startup_task', $managerStart)
if ($audioTaskStart -lt 0 -or $managerStart -lt 0 -or $wifiInit -lt 0 -or $audioCreate -lt 0) {
    throw 'Voice startup task and its early reservation must exist.'
}
if ($audioCreate -ge $wifiInit) {
    throw 'audio_startup must reserve its internal stack before Wi-Fi/BLE/network allocations fragment internal RAM.'
}
if ($gateStart -ge 0) {
    throw 'A second late activation-gate task must not allocate audio_startup after the main page is stable.'
}

$audioTaskEnd = $manager.IndexOf('esp_err_t app_manager_start(void)', $audioTaskStart)
$audioTask = $manager.Substring($audioTaskStart, $audioTaskEnd - $audioTaskStart)
if ($audioTask -notmatch 'snapshot\.voice_activation\.activated' -or
    $audioTask -notmatch 'ui_service_is_main_stable\(\)' -or
    $audioTask -notmatch 'time_service_is_initial_location_settled\(\)' -or
    $audioTask -notmatch 'weather_service_is_initial_request_settled\(\)') {
    throw 'The pre-reserved audio task must own the existing activation, main-page, and network-settle gate.'
}
if ($audioTask -match 'xiaozhi_voice_service_init\(' -or
    $audioTask -notmatch 'voice_services_init_task' -or
    $audioTask -notmatch 'audio startup complete; handing voice init to PSRAM task' -or
    $audioTask -notmatch 'vTaskDelete\(NULL\)') {
    throw 'The pre-reserved task must hand off XiaoZhi initialization before releasing its internal stack.'
}
if ($manager -notmatch 'audio startup task reserved before network services') {
    throw 'Early audio task reservation must be visible in serial logs.'
}

Write-Output 'Voice startup reservation contract passed.'
