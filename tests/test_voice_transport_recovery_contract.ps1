$ErrorActionPreference = 'Stop'
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

if ($voice -notmatch 'send_result\s*>\s*0') {
    throw 'Only a positive WebSocket send result may be treated as success.'
}
if ($voice -notmatch 'voice turn transport failure') {
    throw 'Transport failure must identify the affected voice turn.'
}
if ($voice -notmatch 'voice turn ended after Opus transport failure; requesting generated fallback prompt') {
    throw 'A failed Opus packet must end the current turn and answer with the generated fallback prompt.'
}
if ($voice -notmatch 'discard_uplink_queue\(\)') {
    throw 'A failed turn must discard queued Opus packets before returning to wake mode.'
}
if ($voice -match 'esp_websocket_client_send_bin\([\s\S]*?\);\s*if \(send_result < 0\)') {
    throw 'The uplink path must not accept only negative send results as failure.'
}
Write-Output 'Voice transport recovery contract passed.'
