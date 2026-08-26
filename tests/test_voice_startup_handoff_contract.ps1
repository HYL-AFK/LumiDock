$ErrorActionPreference = 'Stop'

$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

$audioStart = $manager.IndexOf('static void audio_startup_task(void *arg)')
$managerStart = $manager.IndexOf('esp_err_t app_manager_start(void)')
$wifiInit = $manager.IndexOf('wifi_service_init()', $managerStart)
$audioCreate = $manager.IndexOf('xTaskCreatePinnedToCore(audio_startup_task', $managerStart)

if ($audioStart -lt 0 -or $managerStart -lt 0 -or $wifiInit -lt 0 -or $audioCreate -lt 0) {
    throw 'The internal audio startup task must exist before network allocation.'
}
if ($audioCreate -ge $wifiInit) {
    throw 'The internal audio startup stack must still be reserved before network allocations.'
}

$audioEnd = $manager.IndexOf('#endif', $audioStart)
if ($audioEnd -lt 0) { throw 'The audio startup task boundary could not be determined.' }
$audioTask = $manager.Substring($audioStart, $audioEnd - $audioStart)
if ($audioTask -match 'xiaozhi_voice_service_init\(' -or
    $audioTask -notmatch 'voice_services_init_task' -or
    $audioTask -notmatch 'audio startup complete; handing voice init to PSRAM task') {
    throw 'audio_startup_task must hand off WSS/pipeline orchestration before releasing its internal stack.'
}
if ($audioTask -notmatch 'vTaskDelete\(NULL\)') {
    throw 'audio_startup_task must release its internal stack only after voice initialization succeeds.'
}

$voiceInitStart = $manager.IndexOf('static void voice_services_init_task(void *arg)')
$voiceInitEnd = $manager.IndexOf('static void audio_startup_task(void *arg)', $voiceInitStart)
if ($voiceInitStart -lt 0 -or $voiceInitEnd -lt 0) {
    throw 'The PSRAM voice initialization task must exist.'
}
$voiceInitTask = $manager.Substring($voiceInitStart, $voiceInitEnd - $voiceInitStart)
if ($voiceInitTask -match 'xiaozhi_audio_pipeline_init\(') {
    throw 'The PSRAM voice initialization task must not initialize AFE/model resources directly.'
}

if ($voice -notmatch '\.task_stack = 4096' -or
    $voice -match 'xTaskCreatePinnedToCoreWithCaps\(esp_websocket_client_task') {
    throw 'The WebSocket client must retain its native 4096-byte internal task stack.'
}
if ($voice -notmatch 'WSS client create failed: internal=%u largest=%u' -or
    $voice -notmatch 'WSS task/transport start failed: internal=%u largest=%u') {
    throw 'WSS startup failures must distinguish client creation from task/transport startup.'
}
$clientCreateStart = $voice.IndexOf('if (s_client == NULL)')
$clientCreateEnd = $voice.IndexOf('}', $clientCreateStart)
if ($clientCreateStart -lt 0 -or $clientCreateEnd -lt 0 -or
    $voice.Substring($clientCreateStart, $clientCreateEnd - $clientCreateStart) -match 'refresh_credentials') {
    throw 'A WSS client allocation failure must not refresh valid activation credentials.'
}

Write-Output 'Voice startup internal-stack contract passed.'
