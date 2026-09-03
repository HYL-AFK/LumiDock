$ErrorActionPreference = 'Stop'
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$iot = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\voice_iot_command_service.c"
$driver = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\i2s_audio_driver.c"

if ($voice -notmatch 'if \(response_turn\)\s*\{\s*s_boot_greeting_reply_deadline_us\s*=\s*0') {
    throw 'A cloud response must suspend the boot greeting deadline while TTS is being generated.'
}
if ($voice -notmatch '(?s)if \(boot_greeting_window_active\) \{\s*s_boot_greeting_reply_deadline_us\s*=\s*esp_timer_get_time\(\) \+\s*XIAOZHI_BOOT_GREETING_REPLY_WINDOW_US') {
    throw 'The boot greeting reply window must be re-armed after each response playback drain.'
}
if ($voice -notmatch 's_boot_greeting_reply_deadline_us\s*=\s*0;\s*s_state\s*=\s*XIAOZHI_VOICE_WAIT_SPEECH;\s*start_listen\(true\)') {
    throw 'Accepted boot-greeting speech must pause the direct-reply timer before audio capture starts.'
}
if ($voice -notmatch 's_state\s*==\s*XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY\s*&&\s*esp_timer_get_time\(\)\s*>=\s*s_boot_greeting_reply_deadline_us') {
    throw 'The boot greeting timer must only expire while the session is idle and waiting for a reply.'
}
$primerTimeout = $voice.IndexOf('wake greeting data timeout:')
if ($primerTimeout -lt 0) { throw 'Wake primer timeout diagnostic is missing.' }
$primerBlockStart = $voice.LastIndexOf('if (s_waiting_for_wake_primer &&', $primerTimeout)
$primerBlockEnd = $voice.IndexOf('if (s_state == XIAOZHI_VOICE_WAIT_RESPONSE', $primerTimeout)
if ($primerBlockStart -lt 0 -or $primerBlockEnd -lt 0) { throw 'Wake primer timeout block is missing.' }
$primerBlock = $voice.Substring($primerBlockStart, $primerBlockEnd - $primerBlockStart)
if ($primerBlock -match 'close_channel\(') { throw 'Wake primer timeout must keep a healthy WSS alive.' }
if ($primerBlock -notmatch 'XIAOZHI_VOICE_WAIT_FOLLOWUP|s_followup_deadline_us') {
    throw 'Wake primer timeout must fall back to a bounded follow-up window.'
}

foreach ($phrase in @('调高音量', '提高音量', '调低音量', '降低音量')) {
    if ($iot -notmatch $phrase) { throw "missing paired volume phrase: $phrase" }
}
if ($driver -notmatch 'ES8311_MIC_INPUT_GAIN_DB 24\.0f') {
    throw 'Microphone gain must use the 24 dB setting for far-field speech pickup.'
}
Write-Output 'Voice follow-up stability contract passed.'
