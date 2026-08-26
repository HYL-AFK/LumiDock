$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

$initStart = $voice.IndexOf('esp_err_t xiaozhi_voice_service_init(void)')
if ($initStart -lt 0) { throw 'Voice service initialization entry point not found.' }
$init = $voice.Substring($initStart)
$prewarm = $init.IndexOf('prewarm_wss();')
$afe = $init.IndexOf('init_audio_pipeline_on_internal_stack()')

if ($prewarm -lt 0 -or $afe -lt 0 -or $prewarm -ge $afe) {
    throw 'WSS prewarm must run before AFE/WakeNet allocation reserves the final internal 4 KiB task block.'
}

if ($voice -notmatch 'if \(s_client != NULL\) \{\s*if \(s_client_started\) \(void\)esp_websocket_client_stop\(s_client\);\s*\(void\)esp_websocket_client_destroy\(s_client\);') {
    throw 'Initialization failure cleanup must destroy an early-prewarmed WSS client.'
}

Write-Output 'WSS prewarm order contract passed.'
