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
