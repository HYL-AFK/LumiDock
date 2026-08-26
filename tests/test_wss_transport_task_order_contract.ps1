$ErrorActionPreference = 'Stop'

$component = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\managed_components\espressif__esp_websocket_client\esp_websocket_client.c"
$start = $component.IndexOf('esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client)')
$startBlock = $component.Substring($start)
$taskCreate = $startBlock.IndexOf('xTaskCreatePinnedToCore(esp_websocket_client_task')
$transportCreate = $startBlock.IndexOf('esp_websocket_client_create_transport(client)')
if ($start -lt 0 -or $taskCreate -lt 0 -or $transportCreate -lt 0 -or $taskCreate -ge $transportCreate) {
    throw 'The native WebSocket task must be created before transport allocation.'
}
if ($component -notmatch 'TRANSPORT_READY_BIT') {
    throw 'The WebSocket task needs an explicit transport-ready gate.'
}
if ($component -notmatch 'TRANSPORT_READY_BIT\s*\|\s*REQUESTED_STOP_BIT') {
    throw 'The WebSocket task must wait for transport readiness or a stop request.'
}
if ($component -notmatch 'xEventGroupSetBits\(client->status_bits,\s*TRANSPORT_READY_BIT\)') {
    throw 'Successful transport creation must release the WebSocket task gate.'
}
if ($component -notmatch 'transport.*failed|Failed to create websocket transport' -or
    $component -notmatch 'REQUESTED_STOP_BIT') {
    throw 'Transport failure must signal and reclaim the already-created task.'
}
if ($component -notmatch 'if \(client->transport != NULL\)\s*\{?\s*esp_transport_close\(client->transport\)') {
    throw 'Task cleanup must tolerate transport creation failure.'
}
if ($component -notmatch 'task_stack' -or $component -notmatch 'websocket task create begin stack=') {
    throw 'The native task stack diagnostic must remain present.'
}

Write-Output 'WSS transport/task order contract passed.'
