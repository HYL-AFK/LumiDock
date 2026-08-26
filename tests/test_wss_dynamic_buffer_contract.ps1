$ErrorActionPreference = 'Stop'

$defaults = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig.defaults"
$sdkconfig = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$managed = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\managed_components\espressif__esp_websocket_client\esp_websocket_client.c"

if ($defaults -notmatch '(?m)^CONFIG_ESP_WS_CLIENT_ENABLE_DYNAMIC_BUFFER=y\s*$') {
    throw 'sdkconfig.defaults must enable dynamic WebSocket RX/TX buffers.'
}
if ($sdkconfig -notmatch '(?m)^CONFIG_ESP_WS_CLIENT_ENABLE_DYNAMIC_BUFFER=y\s*$') {
    throw 'The active sdkconfig must enable dynamic WebSocket RX/TX buffers.'
}
if ($voice -notmatch 'WSS dynamic RX/TX buffers enabled') {
    throw 'Voice startup must log that WebSocket buffers are allocated after task creation.'
}
if ($voice -notmatch '\.task_stack = 4096' -or
    $voice -match 'xTaskCreatePinnedToCoreWithCaps\(esp_websocket_client_task') {
    throw 'WSS must retain the component-native internal 4096-byte task stack.'
}
if ($managed -notmatch '#ifdef CONFIG_ESP_WS_CLIENT_ENABLE_DYNAMIC_BUFFER' -or
    $managed -notmatch 'esp_websocket_new_buf\(') {
    throw 'The managed WebSocket component must provide its built-in dynamic buffer implementation.'
}

Write-Output 'WSS dynamic-buffer contract passed.'
