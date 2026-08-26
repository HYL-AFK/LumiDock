$ErrorActionPreference = 'Stop'

$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

if ($manager -notmatch 'xTaskCreatePinnedToCoreWithCaps\(voice_services_init_task, "voice_services_init", 6144') {
    throw 'Voice services must be handed off to the existing PSRAM initialization task.'
}
if ($manager -notmatch 'audio startup complete; handing voice init to PSRAM task' -or
    $manager -notmatch 'vTaskDelete\(NULL\)') {
    throw 'The internal audio startup task must release its stack after handing off voice initialization.'
}
$voiceInitStart = $voice.IndexOf('esp_err_t xiaozhi_voice_service_init(void)')
$voiceInitEnd = $voice.IndexOf('bool xiaozhi_voice_service_is_ready', $voiceInitStart)
if ($voiceInitStart -lt 0 -or $voiceInitEnd -lt 0) { throw 'Voice initialization entry point is missing.' }
$init = $voice.Substring($voiceInitStart, $voiceInitEnd - $voiceInitStart)

$hello = $init.IndexOf('wait_for_prewarmed_wss()')
$pipelineTask = $init.IndexOf('init_audio_pipeline_on_internal_stack()')
if ($hello -lt 0 -or $pipelineTask -lt 0 -or $pipelineTask -le $hello) {
    throw 'Internal pipeline initialization must start only after WSS hello.'
}
if ($init -match 'xiaozhi_audio_pipeline_init\(') {
    throw 'The PSRAM voice initialization task must not directly initialize AFE/model resources.'
}
if ($voice -notmatch 'static void voice_pipeline_init_task\(void \*arg\)' -or
    $voice -notmatch 'XIAOZHI_PIPELINE_INIT_STACK_BYTES 4096U' -or
    $voice -notmatch 'xTaskCreateStaticPinnedToCore\(' -or
    $voice -notmatch 's_pipeline_init_stack' -or
    $voice -notmatch 's_pipeline_init_tcb' -or
    $voice -notmatch 'xiaozhi_audio_pipeline_init\(wake_word_detected,\s*vad_changed,\s*opus_frame_ready') {
    throw 'The dedicated internal pipeline task must use static internal storage and initialize the audio pipeline.'
}
if ($voice -notmatch 'voice pipeline init task result=' -or
    $voice -notmatch 'voice pipeline init internal stack watermark=' -or
    $voice -notmatch 'voice pipeline ready after internal-stack model init') {
    throw 'Internal pipeline initialization must expose result and stack diagnostics.'
}
if ($voice -notmatch 's_pipeline_ready = true' -or
    $voice.IndexOf('s_pipeline_ready = true') -lt $voice.IndexOf('init_audio_pipeline_on_internal_stack')) {
    throw 'Pipeline readiness must be published only after internal-stack initialization succeeds.'
}

Write-Output 'Voice internal pipeline stack contract passed.'
