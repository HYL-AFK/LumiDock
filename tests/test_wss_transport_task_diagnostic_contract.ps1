$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$component = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\managed_components\espressif__esp_websocket_client\esp_websocket_client.c"

if ($voice -notmatch 'WSS diagnostic build: transport-task-boundary-v1') {
    throw 'Application WSS startup must identify the transport/task boundary diagnostic build.'
}
foreach ($marker in @('WSS header Authorization', 'WSS header Protocol-Version',
                     'WSS header Client-Id', 'WSS header Device-Id',
                     'WSS register events')) {
    if ($voice -notmatch [regex]::Escape($marker)) {
        throw "Application WSS startup is missing the '$marker' boundary log."
    }
}
foreach ($marker in @('websocket start: transport begin',
                     'websocket transport result=',
                     'websocket task create begin',
                     'websocket task create result=')) {
    if ($component -notmatch [regex]::Escape($marker)) {
        throw "Managed WebSocket startup is missing the '$marker' boundary log."
    }
}
if ($component -notmatch 'heap_caps_get_largest_free_block\(MALLOC_CAP_INTERNAL\)' -or
    $component -notmatch 'xTaskCreatePinnedToCore\(') {
    throw 'Transport/task diagnostics must report internal contiguous memory around the native task creation.'
}
if ($voice -notmatch '\.task_stack = 4096' -or
    $voice -match 'xTaskCreatePinnedToCoreWithCaps\(esp_websocket_client_task') {
    throw 'The native WebSocket task must remain an internal 4096-byte task.'
}
if ($voice -match 'recovered failed Opus packet|preserve_session_for_wss_reconnect|RECONNECTING_FOLLOWUP') {
    throw 'WSS diagnostics must not reintroduce cross-connection Opus recovery.'
}

Write-Output 'WSS transport/task diagnostic contract passed.'
