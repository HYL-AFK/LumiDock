$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
if ($voice -notmatch 'XIAOZHI_PREWARM_READY_TIMEOUT_US \(30LL \* 1000LL \* 1000LL\)') {
    throw 'WSS prewarm must allow the delayed public-server hello for up to 30 seconds.'
}
$initStart = $voice.IndexOf('esp_err_t xiaozhi_voice_service_init(void)')
if ($initStart -lt 0) { throw 'XiaoZhi voice initialization entry point is missing.' }
$init = $voice.Substring($initStart)

$prepare = $init.IndexOf('xiaozhi_audio_pipeline_prepare_encoder()')
$prewarm = $init.IndexOf('prewarm_wss();')
$session = $init.IndexOf('xTaskCreatePinnedToCoreWithCaps(voice_session_task')
$wait = $init.IndexOf('wait_for_prewarmed_wss()')
$uplink = $init.IndexOf('xTaskCreatePinnedToCoreWithCaps(voice_uplink_tx_task')
$afe = $init.IndexOf('init_audio_pipeline_on_internal_stack()')
if ($prepare -lt 0 -or $prewarm -lt 0 -or $session -lt 0 -or $wait -lt 0 -or
    $uplink -lt 0 -or $afe -lt 0) {
    throw 'Voice initialization must include encoder preparation, WSS prewarm, session TX tasks, hello wait, and AFE initialization.'
}
if (-not ($prepare -lt $prewarm -and $prewarm -lt $session -and $session -lt $wait -and
          $wait -lt $uplink -and $uplink -lt $afe)) {
    throw 'WSS must start before voice tasks consume internal memory; the session task must consume hello events before waiting, then uplink and AFE start after hello.'
}

if ($voice -notmatch 'WSS client allocation begin: internal=%u largest=%u' -or
    $voice -notmatch 'WSS client allocation complete: internal=%u largest=%u') {
    throw 'WSS startup must report memory before and after client allocation.'
}
if ($voice -notmatch '\.task_stack = 4096' -or
    $voice -match 'xTaskCreatePinnedToCoreWithCaps\(esp_websocket_client_task') {
    throw 'The WebSocket task must remain the component-native 4096-byte internal stack.'
}

Write-Output 'Voice WSS allocation order contract passed.'
