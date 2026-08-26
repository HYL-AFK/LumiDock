$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_voice_service.c')

if ($voice -match '"mode",\s*"realtime"' -or $voice -match 'mode\\":\\"realtime') {
    throw 'A single-microphone pipeline without AEC must not send realtime listen/start.'
}

if ($voice -notmatch '"mode",\s*"manual"') {
    throw 'listen/start must use manual mode because local VAD owns listen/stop.'
}

if ($voice -match 'RECONNECTING_FOLLOWUP|preserve_session_for_wss_reconnect|s_failed_uplink_packet|recovered failed Opus packet') {
    throw 'A failed turn must not cross a new WSS connection or replay old Opus data.'
}

$listenStartBegin = $voice.IndexOf('static bool send_listen_start(void)')
$listenStartEnd = $voice.IndexOf('static bool send_listen_stop(void)', $listenStartBegin)
if ($listenStartBegin -lt 0 -or $listenStartEnd -lt 0) {
    throw 'listen/start helper is missing.'
}
$listenStart = $voice.Substring($listenStartBegin, $listenStartEnd - $listenStartBegin)
if ($listenStart -notmatch 's_session_id\[0\]' -or
    $listenStart -notmatch '"mode",\s*"manual"' -or
    $listenStart -notmatch 'listen/start sent') {
    throw 'listen/start must require the current session_id and log the control frame.'
}

$listenStopBegin = $voice.IndexOf('static bool send_listen_stop(void)')
$listenStopEnd = $voice.IndexOf('static void start_listen(bool enable_capture)', $listenStopBegin)
if ($listenStopBegin -lt 0 -or $listenStopEnd -lt 0) {
    throw 'listen/stop helper is missing.'
}
$listenStop = $voice.Substring($listenStopBegin, $listenStopEnd - $listenStopBegin)
if ($listenStop -notmatch 'session_id' -or $listenStop -notmatch 'listen/stop sent') {
    throw 'listen/stop must carry the current session_id and log the control frame.'
}

if ($voice -match 'if \(is_followup_reconnect_state\(\)\s*&&') {
    throw 'The session task must not repeatedly enter a follow-up reconnect path.'
}

if ($voice -notmatch 'boot greeting PCM drained; direct reply window=3min') {
    throw 'The active greeting must still open its three-minute direct reply window.'
}

Write-Output 'Single-connection voice contract passed.'
