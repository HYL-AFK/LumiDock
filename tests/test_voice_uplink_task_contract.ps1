$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$playout = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\audio_playback_service.c"
$driver = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\i2s_audio_driver.c"

if ($voice -notmatch '#define XIAOZHI_UPLINK_QUEUE_BYTES \(64U \* 1024U\)') {
    throw 'Opus uplink queue must be expanded to 64 KiB PSRAM.'
}
if ($voice -notmatch 'static void voice_uplink_tx_task\(void \*arg\)') {
    throw 'Opus uplink must run in an independent TX task.'
}
if ($voice -notmatch 'xTaskCreatePinnedToCoreWithCaps\(voice_uplink_tx_task') {
    throw 'The voice service must create the independent uplink TX task.'
}
$sessionStart = $voice.IndexOf('static void voice_session_task')
$sessionEnd = $voice.IndexOf('static bool wait_for_prewarmed_wss', $sessionStart)
if ($sessionStart -lt 0 -or $sessionEnd -lt 0) { throw 'voice_session_task boundaries are missing.' }
$sessionBlock = $voice.Substring($sessionStart, $sessionEnd - $sessionStart)
if ($sessionBlock -match 'drain_uplink\(' -or $sessionBlock -match 'wait_for_uplink_slot\(') {
    throw 'voice_session_task must not drain or pace Opus frames.'
}
if ($voice -notmatch 'XIAOZHI_UPLINK_TAIL_FLUSH_FRAMES 2U' -or
    $voice -notmatch 'XIAOZHI_UPLINK_TAIL_FLUSH_TIMEOUT_US \(150LL \* 1000LL\)') {
    throw 'VAD_END must use the fixed two-frame/150 ms tail flush contract.'
}
$vadEndStart = $voice.IndexOf('case VOICE_EVENT_VAD_END:')
$networkStart = $voice.IndexOf('case VOICE_EVENT_NETWORK_LOST:', $vadEndStart)
if ($vadEndStart -lt 0 -or $networkStart -lt 0) { throw 'VAD_END state block is missing.' }
$vadEndBlock = $voice.Substring($vadEndStart, $networkStart - $vadEndStart)
if ($vadEndBlock -notmatch 'finish_listen_with_tail_flush()') {
    throw 'VAD_END must finish listening through the bounded tail-flush path.'
}
$finishStart = $voice.IndexOf('static void finish_listen_with_tail_flush')
$detectStart = $voice.IndexOf('static void send_detect', $finishStart)
if ($finishStart -lt 0 -or $detectStart -lt 0) { throw 'Bounded listen finish helper is missing.' }
$finishBlock = $voice.Substring($finishStart, $detectStart - $finishStart)
if ($finishBlock -notmatch 'xiaozhi_audio_pipeline_set_streaming\(false\)' -or
    $finishBlock -notmatch 'begin_terminal_uplink_transition\(\)' -or
    $finishBlock -notmatch 'wait_for_main_uplink_write\(\)' -or
    $finishBlock -notmatch 'capture_terminal_uplink_tails\(\)' -or
    $finishBlock -notmatch 'send_terminal_uplink_sequence\(\)') {
    throw 'listen/stop must stop capture, isolate terminal tails after main TX, and use the terminal owner.'
}
if ($playout -notmatch '#define AUDIO_PLAYOUT_QUEUE_DEPTH 128U') {
    throw 'PCM playout queue must be expanded to 128 30-ms chunks.'
}
if ($driver -notmatch 's_ctrl_lock' -or $driver -notmatch 's_rx_lock' -or $driver -notmatch 's_tx_lock') {
    throw 'ES8311 driver must split control, RX, and TX locks.'
}
if ($driver -match 'esp_codec_dev_read[sS]{0,220}s_ctrl_lock' -or
    $driver -match 's_ctrl_lock[sS]{0,220}esp_codec_dev_read') {
    throw 'Blocking ES8311 read must not use the codec control lock.'
}
if ($driver -match 'esp_codec_dev_write[sS]{0,220}s_ctrl_lock' -or
    $driver -match 's_ctrl_lock[sS]{0,220}esp_codec_dev_write') {
    throw 'Blocking ES8311 write must not use the codec control lock.'
}

Write-Output 'Voice independent uplink and audio lock contract passed.'
