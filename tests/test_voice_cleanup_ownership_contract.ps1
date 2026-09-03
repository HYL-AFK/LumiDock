$ErrorActionPreference = 'Stop'
$voice = Get-Content (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_voice_service.c') -Raw

if ($voice -notmatch 's_client_cleanup_in_progress') {
    throw 'WSS cleanup must have an explicit ownership guard.'
}
$closeStart = $voice.IndexOf('static void close_channel(bool refresh_credentials)')
$sendStart = $voice.IndexOf('static bool send_uplink_packet')
if ($closeStart -lt 0 -or $sendStart -lt 0) { throw 'WSS lifecycle functions are missing.' }
$close = $voice.Substring($closeStart, $sendStart - $closeStart)
if ($close -notmatch 's_client_cleanup_in_progress') {
    throw 'close_channel must claim cleanup ownership before stop/destroy.'
}
if ($voice -notmatch 's_client = NULL;[\s\S]{0,120}s_client_started = false') {
    throw 'WSS cleanup must clear client and started state together.'
}
if ($voice -notmatch 'voice turn transport failure; no cloud fallback, entering bounded cleanup[\s\S]{0,220}play_error_tone_then_close\(false\)') {
    throw 'Transport failure must converge on bounded local cleanup.'
}
Write-Output 'Voice cleanup ownership contract passed.'
