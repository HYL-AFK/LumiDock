$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_voice_service.c')
$pipeline = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_audio_pipeline.c')
$pipelineHeader = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_audio_pipeline.h')

if ($voice -match 'RECONNECTING_FOLLOWUP|preserve_session_for_wss_reconnect|s_failed_uplink_packet|recovered failed Opus packet') {
    throw 'An active turn must not be resumed by replaying old Opus on a replacement WSS.'
}
if ($voice -notmatch 'post_event_value|event->value\s*!=\s*s_wss_connection_seq') {
    throw 'WSS events must carry and validate their originating connection sequence.'
}
if ($voice -notmatch 'receive_websocket_payload[\s\S]{0,300}connection_seq\s*!=\s*s_wss_connection_seq') {
    throw 'Binary/text data from an old WSS connection must be discarded before reassembly.'
}
if ($voice -notmatch 's_session_id' -or
    $voice -notmatch 'GetObjectItemCaseSensitive\(root, "session_id"\)' -or
    $voice -notmatch 'listen/start sent[\s\S]{0,120}session_id') {
    throw 'The active server session_id must be captured and included in listen/start.'
}
if ($voice -notmatch 'discard_uplink_queue|reset_after_transport_failure') {
    throw 'A failed turn must clear stale uplink data and reset the local audio stream.'
}
if ($voice -notmatch 'follow-up WSS health check failed after PCM drain; ending turn') {
    throw 'Playback drain must end a dead-session follow-up instead of opening a replacement WSS.'
}
if ($pipeline -notmatch 'reset_after_transport_failure' -or
    $pipelineHeader -notmatch 'reset_after_transport_failure') {
    throw 'The audio pipeline must expose transport-failure cleanup without disabling WakeNet.'
}

$vadStart = $voice.IndexOf('if (s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY)')
$vadEnd = $voice.IndexOf('} else if (s_state == XIAOZHI_VOICE_WAIT_FOLLOWUP', $vadStart)
if ($vadStart -lt 0 -or $vadEnd -lt 0) { throw 'Boot greeting VAD boundary is missing.' }
$vadBlock = $voice.Substring($vadStart, $vadEnd - $vadStart)
if ($vadBlock -match 'publish_ui_state\(VOICE_UI_CONNECTING\)|start_wss\(') {
    throw 'Speech inside the active boot greeting window must not enter connecting or start a new WSS session.'
}
if ($vadBlock -notmatch 's_boot_greeting_reply_deadline_us\s*=\s*0;\s*s_state\s*=\s*XIAOZHI_VOICE_WAIT_SPEECH') {
    throw 'Starting the direct-reply turn must pause its idle-only deadline before capture begins.'
}
if ($voice -notmatch 's_boot_greeting_reply_deadline_us != 0[\s\S]{0,260}s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY') {
    throw 'The boot-greeting deadline must run only while the session is idle and waiting for a reply.'
}

Write-Output 'Active single-session contract passed.'
