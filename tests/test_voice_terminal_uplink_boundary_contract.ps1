$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

@(
    @{ Pattern = 'static volatile bool s_uplink_terminal_active;'; Message = 'VAD_END requires an explicit terminal-uplink ownership gate.' },
    @{ Pattern = 'static volatile uint32_t s_uplink_terminal_generation;'; Message = 'Terminal uplink ownership must be generation-tagged for diagnostics.' },
    @{ Pattern = 'static volatile bool s_uplink_bin_in_flight;'; Message = 'Terminal transition must distinguish an already-started binary write.' },
    @{ Pattern = 'static volatile bool s_uplink_terminal_close_pending;'; Message = 'A terminal deadline must defer closure until an in-flight main write is safe to close.' },
    @{ Pattern = 'static volatile bool s_uplink_terminal_listen_stop_started;'; Message = 'Disconnect diagnostics need a persistent terminal listen/stop boundary marker.' },
    @{ Pattern = 'static uint8_t s_terminal_tail_frames\[XIAOZHI_UPLINK_TAIL_FLUSH_FRAMES\]\[XIAOZHI_MAX_OPUS_FRAME_BYTES\];'; Message = 'Terminal tails must have a dedicated captured buffer.' },
    @{ Pattern = 'static uint8_t s_terminal_tail_count;'; Message = 'Terminal tails must track their captured count independently of the generic queue.' },
    @{ Pattern = 'static bool capture_terminal_uplink_tails\(void\)'; Message = 'VAD_END requires an explicit generic-queue capture helper.' },
    @{ Pattern = 'static void log_uplink_queue_depth\(const char \*phase\)'; Message = 'Terminal cutoff must log actual generic queue depth.' },
    @{ Pattern = 'static bool send_terminal_uplink_sequence\(void\)'; Message = 'One dedicated terminal sender must own tails and listen/stop.' }
) | ForEach-Object {
    if ($voice -notmatch $_.Pattern) { throw $_.Message }
}

$txStart = $voice.IndexOf('static void voice_uplink_tx_task')
$txEnd = $voice.IndexOf('static void handle_event', $txStart)
if ($txStart -lt 0 -or $txEnd -lt 0) { throw 'TX task boundaries are missing.' }
$txBlock = $voice.Substring($txStart, $txEnd - $txStart)
if ($txBlock -notmatch 'begin_main_uplink_send\(\)' -or $txBlock -notmatch '"bin-main"') {
    throw 'The normal TX task must acquire explicit main-binary admission and label its operation.'
}
if ($txBlock -match 's_uplink_tail_requested|VOICE_EVENT_UPLINK_TAIL_FLUSHED') {
    throw 'The normal TX task must not own terminal tails or post terminal completion events.'
}
if ($txBlock -notmatch 'const bool terminal_deadline = finish_main_uplink_send\(\)' -or
    $txBlock -match 'post_event\(VOICE_EVENT_UPLINK_FAILED\)' -or
    $txBlock -notmatch 'terminal main write completed after deadline') {
    throw 'Main TX must complete the deferred terminal boundary without racing the terminal owner with a failure event.'
}

$waitStart = $voice.IndexOf('static bool wait_for_main_uplink_write')
$captureStart = $voice.IndexOf('static bool capture_terminal_uplink_tails', $waitStart)
if ($waitStart -lt 0 -or $captureStart -lt 0) { throw 'Bounded main-write wait helper is missing.' }
$waitBlock = $voice.Substring($waitStart, $captureStart - $waitStart)
if ($waitBlock -notmatch 's_uplink_tail_deadline_us' -or
    $waitBlock -notmatch 'XIAOZHI_UPLINK_MAIN_WRITE_WAIT_TIMEOUT_US' -or
    $waitBlock -notmatch 'in_flight' -or
    $waitBlock -notmatch 'wait_us') {
    throw 'In-flight main-write wait must keep the 150 ms tail cutoff separate from its bounded write wait and log the wait.'
}

$captureEnd = $voice.IndexOf('static void reset_uplink_pacing', $captureStart)
if ($captureEnd -lt 0) { throw 'Terminal tail capture boundaries are missing.' }
$captureBlock = $voice.Substring($captureStart, $captureEnd - $captureStart)
if ($voice -notmatch 'vRingbufferGetInfo\(' -or
    $captureBlock -notmatch 'log_uplink_queue_depth\("terminal-capture-before"\)' -or
    $captureBlock -notmatch 'log_uplink_queue_depth\("terminal-capture-after"\)') {
    throw 'Terminal tail capture must report actual Ringbuffer depth before and after draining generic frames.'
}

$finishStart = $voice.IndexOf('static void finish_listen_with_tail_flush')
$detectStart = $voice.IndexOf('static void send_detect', $finishStart)
if ($finishStart -lt 0 -or $detectStart -lt 0) { throw 'VAD_END terminal helper boundaries are missing.' }
$finishBlock = $voice.Substring($finishStart, $detectStart - $finishStart)
if ($finishBlock -notmatch 'xiaozhi_audio_pipeline_set_streaming\(false\)' -or
    $finishBlock -notmatch 'begin_terminal_uplink_transition\(\)' -or
    $finishBlock -notmatch 'wait_for_main_uplink_write\(\)' -or
    $finishBlock -notmatch 'capture_terminal_uplink_tails\(\)' -or
    $finishBlock -notmatch 'send_terminal_uplink_sequence\(\)') {
    throw 'STOPPING must stop capture, close main TX admission, wait in-flight main TX, capture tails, then use the terminal sender.'
}
if ($finishBlock -notmatch 'if \(!wait_for_main_uplink_write\(\)\)' -or
    $finishBlock -notmatch 'terminal main write wait timed out' -or
    $finishBlock -notmatch 's_last_uplink_send_result' -or
    $finishBlock -notmatch 's_last_uplink_send_connected') {
    throw 'STOPPING must wait for an admitted main write with a bounded total timeout and report its result and connection state.'
}
if ($finishBlock -match 'stop_listen\(|wait_for_uplink_tail_flush|s_uplink_tail_requested') {
    throw 'STOPPING must not return to the old generic tail-flush or separate stop-owner path.'
}

$producerStart = $voice.IndexOf('static void opus_frame_ready')
$initStart = $voice.IndexOf('esp_err_t xiaozhi_voice_service_init', $producerStart)
if ($producerStart -lt 0 -or $initStart -lt 0) { throw 'Opus producer boundaries are missing.' }
$producerBlock = $voice.Substring($producerStart, $initStart - $producerStart)
if ($producerBlock -notmatch 's_uplink_terminal_active') {
    throw 'Opus producer must reject frames after terminal transition begins.'
}

if ($voice -match 'tail send failed; sending listen/stop fallback|VOICE_EVENT_UPLINK_TAIL_FLUSHED|wait_for_uplink_tail_flush') {
    throw 'Terminal tail failures must close the turn rather than use the old same-connection fallback race.'
}

$wssStart = $voice.IndexOf('static void websocket_event')
$wakeStart = $voice.IndexOf('static void wake_word_detected', $wssStart)
if ($wssStart -lt 0 -or $wakeStart -lt 0) { throw 'WSS callback boundaries are missing.' }
$wssBlock = $voice.Substring($wssStart, $wakeStart - $wssStart)
if ($wssBlock -notmatch 'event_id=%ld' -or $wssBlock -notmatch 'terminal_gen=%u' -or
    $wssBlock -notmatch 's_uplink_tx_operation' -or
    $wssBlock -notmatch 'post_wss_disconnect_event\(connection_seq, event_id\)') {
    throw 'WSS callback diagnostics must log raw event id, terminal generation, and current TX operation.'
}

$disconnectStart = $voice.IndexOf('case VOICE_EVENT_WSS_DISCONNECTED:')
$textStart = $voice.IndexOf('case VOICE_EVENT_WSS_TEXT:', $disconnectStart)
if ($disconnectStart -lt 0 -or $textStart -lt 0) { throw 'WSS disconnect state handling is missing.' }
$disconnectBlock = $voice.Substring($disconnectStart, $textStart - $disconnectStart)
if ($disconnectBlock -notmatch 'event->detail' -or $disconnectBlock -notmatch 'disconnect_before_listen_stop') {
    throw 'WSS disconnect state logs must retain the raw event id and state whether it preceded listen/stop.'
}
if ($disconnectBlock -notmatch 's_uplink_terminal_listen_stop_started') {
    throw 'Disconnect-before-listen/stop logging must use the persistent terminal boundary marker.'
}

Write-Output 'Voice terminal uplink boundary contract passed.'
