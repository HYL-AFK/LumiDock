$ErrorActionPreference = 'Stop'

$wifi = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\network\wifi_service.c"
$wifiManager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\wifi_manager.c"
$blufi = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\network\ble_provision.c"
$app = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"
$defaults = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig.defaults"
$config = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig"

if ($defaults -notmatch 'CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096' -or
    $config -notmatch 'CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096') {
    throw 'The system event task stack must be 4096 in defaults and the active sdkconfig.'
}

$handler = [regex]::Match($wifiManager, '(?s)static void wifi_event_handler\(.*?\n\}')
if (-not $handler.Success -or $handler.Value -match 'wifi_service|xiaozhi_|ble_provision_|mid_snapshot|schedule_sta_reconnect|esp_wifi_connect') {
    throw 'Wi-Fi default event callback must only capture state and notify the async dispatcher.'
}

if ($blufi -match 'static void blufi_wifi_event_handler|esp_event_handler_register\(WIFI_EVENT|esp_event_handler_register\(IP_EVENT') {
    throw 'BluFi must not register a second Wi-Fi/IP event callback.'
}

if ($wifi -match 'wifi_status_dispatch_task|xTaskCreatePinnedToCoreWithCaps\(wifi_status_dispatch_task' -or
    $blufi -match 'blufi_event_dispatch_task|xTaskCreatePinnedToCoreWithCaps\(blufi_event_dispatch_task') {
    throw 'Network status and BluFi work must not create separate dispatch tasks.'
}
if ($wifiManager -notmatch 'network_coordinator_task' -or
    $wifiManager -notmatch 'xTaskCreatePinnedToCore\(network_coordinator_task') {
    throw 'All network work must run in one internal-RAM network coordinator task.'
}
if ($wifiManager -match 'xTaskCreatePinnedToCoreWithCaps\(' -or
    $wifiManager -match 'wifi_event_handler[\s\S]*runtime_event_log_append|wifi_event_handler[\s\S]*app_config_save') {
    throw 'Wi-Fi event path must not create PSRAM tasks or perform persistence in sys_evt.'
}

if ($app -notmatch 'i2s_audio_driver_init_lock' -or
    $app -notmatch 'xSemaphoreTake\(i2s_audio_driver_init_lock') {
    throw 'Early and deferred audio initialization must share one ownership lock.'
}

Write-Output 'Wi-Fi event dispatch contract passed.'
