$ErrorActionPreference = 'Stop'
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

if ($voice -notmatch 's_wss_connection_seq') {
    throw 'WSS diagnostics must identify each client connection instance.'
}
if ($voice -notmatch 's_state == XIAOZHI_VOICE_LISTENING') {
    throw 'The voice service must identify an active listening turn when transport drops.'
}
if ($voice -notmatch 'WSS callback event_id=%ld connection_seq=%u.*terminal_gen=%u') {
    throw 'WSS callback diagnostics must include raw event id, connection sequence, and terminal generation.'
}
# 但这条日志不能每个事件都打：WEBSOCKET_EVENT_DATA 在 TTS 期间每 40~90 ms 就来一次，
# 实测 247 行的日志里它独占 52 行（21%），把 TTS/电台的关键行全淹了。
# 现在只有“不是 DATA 事件”或“五元组变了”才输出。
if ($voice -notmatch 'if \(event_id != WEBSOCKET_EVENT_DATA \|\| state_changed\)') {
    throw 'The WSS callback trace must stay silent for steady-state DATA events.'
}
if ($voice -notmatch 'uplink_operation_equal') {
    throw 'The op name must be compared by content; string literal addresses are not guaranteed unique.'
}
if ($voice -match 'failed_uplink_packet_len|recovered failed Opus packet|preserve_session_for_wss_reconnect') {
    throw 'A failed Opus packet must not cross a WSS reconnect.'
}
if ($voice -notmatch 'stale started-but-disconnected WSS client|esp_websocket_client_is_connected\(s_client\)') {
    throw 'A stale started-but-disconnected client must be replaced before a new wake session.'
}
if ($voice -notmatch 'voice turn ended after Opus transport failure') {
    throw 'A failed listening session must end cleanly and require a new wake word.'
}
Write-Output 'Voice connection lifetime contract passed.'
