$ErrorActionPreference = 'Stop'
$startup = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\startup_audio.c"
$playout = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\audio_playback_service.c"
$driver = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\i2s_audio_driver.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

if ($startup -notmatch 'AUDIO_PLAYOUT_SOURCE_STARTUP,\s*s_startup_epoch,\s*64U') {
    throw 'Startup PCM must wait for a 64-frame prebuffer before enabling playback.'
}
if ($playout -notmatch 's_playout_silence' -or
    $playout -notmatch 'PCM underrun silence write failed') {
    throw 'Startup and TTS underruns must hold the output state and write zero PCM instead of mute/unmute.'
}
if ($driver -notmatch 'ES8311.*pre.?roll|s_output_pre.?roll|silence.*960') {
    throw 'The first PCM frame needs a muted silence pre-roll before DAC unmute.'
}
if ($voice -match 'reconnect.*follow.?up|follow.?up.*reconnect|preserve.*session|WSS reconnect hello complete') {
    throw 'Follow-up disconnects must not start a session-preserving WSS recovery path.'
}
if ($voice -notmatch 'disconnect.*state=.*turn=|WSS.*disconnect.*turn=') {
    throw 'WSS disconnect diagnostics must include state and turn identifiers.'
}
if ($voice -notmatch 'disable_auto_reconnect\s*=\s*true') {
    throw 'The application must end a disconnected voice turn without an implicit WSS reconnect.'
}
if ($voice -notmatch 'follow-up WSS health check failed after PCM drain; ending turn') {
    throw 'A dead WSS after TTS drain must end the turn instead of reconnecting it.'
}
Write-Output 'Audio noise and session recovery contract passed.'
