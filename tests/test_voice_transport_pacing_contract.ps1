$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$defaults = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig.defaults"
$config = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig"

if ($defaults -notmatch '(?m)^CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y$' -or
    $config -notmatch '(?m)^CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y$') {
    throw 'WebSocket TX must use a separate lock so sending cannot block the receive task.'
}
if ($defaults -notmatch '(?m)^CONFIG_ESP_WS_CLIENT_TX_LOCK_TIMEOUT_MS=2000$' -or
    $config -notmatch '(?m)^CONFIG_ESP_WS_CLIENT_TX_LOCK_TIMEOUT_MS=2000$') {
    throw 'WebSocket TX lock needs the documented 2 second acquisition timeout.'
}
if ($voice -match 'esp_websocket_client_send_bin\([\s\S]*?pdMS_TO_TICKS\(100\)') {
    throw 'Opus uplink must not fail after the current 100 ms send timeout.'
}
if ($voice -notmatch 'XIAOZHI_OPUS_FRAME_INTERVAL_US\s*\(60LL\s*\*\s*1000LL\)') {
    throw 'Uplink pacing must use the negotiated 60 ms Opus frame interval.'
}
if ($voice -notmatch 's_next_uplink_deadline_us') {
    throw 'Uplink pacing must maintain a per-turn monotonic send deadline.'
}
if ($voice -notmatch 'voice_uplink_tx_task[\s\S]*s_next_uplink_deadline_us[\s\S]*send_uplink_packet\(') {
    throw 'The independent uplink task must wait for its realtime send deadline before transmission.'
}
if ($voice -notmatch 'voice_uplink_tx_task[\s\S]*send_uplink_packet\([\s\S]*?\)[\s\S]*advance_uplink_pacing\(\)') {
    throw 'The send deadline must advance only after a successful Opus packet send.'
}
if ($voice -notmatch 'send_listen_start\(\)\)\s*\{[\s\S]{0,300}reset_uplink_pacing\(\)') {
    throw 'A new listen/start turn must reset the previous uplink deadline.'
}
Write-Output 'Voice transport pacing contract passed.'
