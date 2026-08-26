$time = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_time.c"
$weather = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_weather.c"
$sdkconfig = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig"
$partitions = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\partitions.csv"
$wifiManager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\wifi_manager.c"
$wifiHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\wifi_manager.h"
$wifiService = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\network\wifi_service.c"
$snapshot = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\mid_snapshot.h"
$statusLed = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\status_led_service.c"

if ($time -notmatch 'static void apply_sntp_time\(void\)') {
    throw 'SNTP completion handler is missing.'
}

$applyStart = $time.IndexOf('static void apply_sntp_time(void)')
$applyEnd = $time.IndexOf('static void schedule_sntp_retry', $applyStart)
$applyBody = $time.Substring($applyStart, $applyEnd - $applyStart)
if ($applyBody -notmatch 'xEventGroupSetBits\(s_location_events, TIME_NET_EVENT\)') {
    throw 'Successful SNTP must release the location task.'
}

$notifyStart = $time.IndexOf('void app_time_notify_network(void)')
$notifyBody = $time.Substring($notifyStart)
if ($notifyBody -match 'xEventGroupSetBits\(s_location_events, TIME_NET_EVENT\)') {
    throw 'A raw network event must not start location before SNTP has synchronized the clock.'
}

$locationStart = $time.IndexOf('static void location_task(void *arg)')
$locationEnd = $time.IndexOf('esp_err_t app_time_init(void)', $locationStart)
$locationBody = $time.Substring($locationStart, $locationEnd - $locationStart)
$locationRefresh = $locationBody.IndexOf('refresh_location(&changed)')
$weatherNotify = $locationBody.LastIndexOf('app_weather_notify_network()')
if ($locationRefresh -lt 0 -or $weatherNotify -lt 0 -or $weatherNotify -lt $locationRefresh) {
    throw 'Weather must be notified only after the location request has completed.'
}

if ($weather -notmatch '#include "time_snapshot.h"' -or
    $weather -notmatch 'time_snapshot_is_ready\(\)') {
    throw 'Weather requests must require an SNTP-synchronized system clock.'
}

if ($sdkconfig -notmatch 'CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y' -or
    $sdkconfig -notmatch '# CONFIG_MBEDTLS_DYNAMIC_BUFFER is not set') {
    throw 'TLS allocation must use the confirmed PSRAM policy on the N8R2 board.'
}
if ($sdkconfig -notmatch 'CONFIG_LV_USE_CLIB_MALLOC=y' -or
    $sdkconfig -match 'CONFIG_LV_USE_BUILTIN_MALLOC=y') {
    throw 'LVGL must use ESP-IDF malloc/PSRAM instead of the fixed internal pool required by the former UI.'
}
if ($partitions -notmatch 'factory,\s+app,\s+factory,\s+0x10000,\s+0x770000,' -or
    $partitions -notmatch 'model,\s+data,\s+spiffs,\s*,\s*0x80000,') {
    throw 'The 8 MB flash layout must reserve a 7.5 MB app slot and a 512 KB ESP-SR model partition.'
}

if ($wifiManager -notmatch 'WIFI_STA_FAILURE_LIMIT 5U' -or
    $wifiManager -notmatch 'wifi_manager_enable_sta_reconnect\(false\)') {
    throw 'STA retries must stop after five consecutive connection failures.'
}
if ($wifiHeader -notmatch 'wifi_manager_is_sta_reconnect_enabled') {
    throw 'The Wi-Fi reconnect state must be exposed to snapshot consumers.'
}
if ($snapshot -notmatch 'sta_reconnect_enabled') {
    throw 'The protected snapshot must publish whether STA is still retrying.'
}
if ($wifiService -notmatch 'wifi\.sta_reconnect_enabled = wifi_manager_is_sta_reconnect_enabled\(\)') {
    throw 'Wi-Fi service must publish the reconnect state into the snapshot.'
}
if ($statusLed -notmatch 'snapshot->wifi\.sta_reconnect_enabled') {
    throw 'The strip must not show Wi-Fi connecting after automatic retries are disabled.'
}

Write-Output 'Network service order checks passed.'

