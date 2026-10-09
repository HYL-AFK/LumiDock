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

# ESP32-S3 does not define SOC_ESP_NIMBLE_CONTROLLER, so esp_nimble_init() is a full
# host init: it runs npl_freertos_mempool_init() -> os_mempool_init(&ble_freertos_ev_pool,
# 10, ...) over a pool that only holds ten events. Re-running it while the host task is
# alive re-chains that free list underneath a concurrent os_memblock_get, which panics
# at os_mempool.c:359 (SLIST_NEXT(NULL)) and boot-loops the device. The stack may be
# started once, from ble_provision_init() only. Comments are stripped first so that the
# rationale prose above the code cannot satisfy these assertions.
$blufiCode = [regex]::Replace($blufi, '(?s)/\*.*?\*/', '')
$openWindow = [regex]::Match($blufiCode, '(?s)esp_err_t\s+ble_provision_open_window\(void\)\s*\{.*?\n\}')
if (-not $openWindow.Success -or
    $openWindow.Value -match 'blufi_stack_init|esp_nimble_init|nimble_port_init|nimble_port_deinit') {
    throw 'ble_provision_open_window() must only manage the window; starting the BLE stack there re-inits NimBLE twice and reboot-loops the device.'
}
$stackInitCalls = ([regex]::Matches($blufiCode, 'blufi_stack_init\(\)')).Count
if ($stackInitCalls -ne 1) {
    throw "The BLE stack must be started from exactly one call site; found $stackInitCalls."
}
if ($blufiCode -match 'nimble_port_deinit|esp_nimble_deinit|ble_hs_deinit') {
    throw 'BluFi must not tear the NimBLE host down at runtime: ESP32-S3 cannot re-initialize it in-process.'
}

if ($wifi -match 'wifi_status_dispatch_task|xTaskCreatePinnedToCoreWithCaps\(wifi_status_dispatch_task' -or
    $blufi -match 'blufi_event_dispatch_task|xTaskCreatePinnedToCoreWithCaps\(blufi_event_dispatch_task') {
    throw 'Network status and BluFi work must not create separate dispatch tasks.'
}
if ($wifiManager -notmatch 'network_coordinator_task' -or
    $wifiManager -notmatch 'xTaskCreatePinnedToCore\(network_coordinator_task') {
    throw 'All network work must run in one internal-RAM network coordinator task.'
}
# 这条断言原本是全文匹配 wifi_manager.c 里的 xTaskCreatePinnedToCoreWithCaps，但该文件在
# sta_reconnect_task / internet_probe_task 上合法地使用 PSRAM 栈（都不在事件回调里），
# 于是断言从写下那天起就永久变红——红线长期存在，真正的回归就再也看不见了。
# 后半句的 'wifi_event_handler[\s\S]*runtime_event_log_append' 同样是贪婪跨全文，等价于
# "handler 之后任意位置出现即失败"，既过宽又与上一条重复。这里按断言语义统一收进
# wifi_event_handler 块内：事件回调只允许做事件映射和投递。
if ($handler.Value -match 'xTaskCreatePinnedToCoreWithCaps\(|runtime_event_log_append|app_config_save') {
    throw 'Wi-Fi event path must not create PSRAM tasks or perform persistence in sys_evt.'
}

if ($app -notmatch 'i2s_audio_driver_init_lock' -or
    $app -notmatch 'xSemaphoreTake\(i2s_audio_driver_init_lock') {
    throw 'Early and deferred audio initialization must share one ownership lock.'
}

Write-Output 'Wi-Fi event dispatch contract passed.'
