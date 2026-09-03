$ErrorActionPreference = 'Stop'

$playoutPath = Join-Path $PSScriptRoot '..\main\MID\system\audio_playback_service.c'
$headerPath = Join-Path $PSScriptRoot '..\main\MID\system\audio_playback_service.h'
if (-not (Test-Path -LiteralPath $playoutPath) -or -not (Test-Path -LiteralPath $headerPath)) {
    throw 'A shared PCM playout service is required for startup audio and cloud TTS.'
}

$playout = Get-Content -Raw -LiteralPath $playoutPath
$header = Get-Content -Raw -LiteralPath $headerPath
$pipeline = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_audio_pipeline.c')
$pipelineHeader = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_audio_pipeline.h')
$voice = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_voice_service.c')
$startup = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\main\APP\startup_audio.c')
$cmake = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\main\CMakeLists.txt')

@(
    @{ Content = $playout; Pattern = '#define AUDIO_PLAYOUT_PCM_CHUNK_BYTES 960U'; Message = 'PCM playout must use 30 ms chunks.' },
    @{ Content = $playout; Pattern = '#define AUDIO_PLAYOUT_TTS_PREBUFFER_FRAMES 120U'; Message = 'Cloud TTS must prebuffer about 3.6 seconds to absorb cloud sentence pauses.' },
    @{ Content = $playout; Pattern = '#define AUDIO_PLAYOUT_QUEUE_DEPTH 128U'; Message = 'PCM playout needs about four seconds of bounded PSRAM headroom.' },
    @{ Content = $playout; Pattern = 'AUDIO_PLAYOUT_EVENT_STARTED'; Message = 'The playout service must report the first committed PCM frame.' },
    @{ Content = $playout; Pattern = 'AUDIO_PLAYOUT_EVENT_FINISHED'; Message = 'The playout service must report drain completion after DAC mute.' },
    @{ Content = $playout; Pattern = 'SemaphoreHandle_t io_lock'; Message = 'I2S writes need a separate serialization lock from playout state.' },
    @{ Content = $playout; Pattern = 'TTS playout underrun'; Message = 'A real PCM starvation must be diagnosed explicitly.' },
    @{ Content = $header; Pattern = 'audio_playback_service_enqueue_pcm'; Message = 'The shared playout service must accept decoded PCM frames.' },
    @{ Content = $pipeline; Pattern = 'audio_playback_service_enqueue_pcm'; Message = 'Decoded TTS PCM must be sent to the shared playout service.' },
    @{ Content = $pipeline; Pattern = 's_pipeline\.tts_input_closed'; Message = 'TTS stop must close input without discarding decoded tail audio.' },
    @{ Content = $pipeline; Pattern = 'xiaozhi_audio_pipeline_resume_after_playback'; Message = 'The audio pipeline must expose a fresh post-playback VAD baseline.' },
    @{ Content = $pipelineHeader; Pattern = 'xiaozhi_tts_playback_cb_t'; Message = 'Voice service must receive asynchronous PCM playout events.' },
    @{ Content = $voice; Pattern = 'XIAOZHI_VOICE_TTS_BUFFERING'; Message = 'Accepted TTS must remain distinct from audible speaking.' },
    @{ Content = $voice; Pattern = 'VOICE_EVENT_TTS_PLAYBACK_STARTED'; Message = 'SPEAKING must be driven by an actual playout-start event.' },
    @{ Content = $voice; Pattern = 'VOICE_EVENT_TTS_PLAYBACK_FINISHED'; Message = 'Follow-up listening must wait for PCM drain and hardware mute.' },
    @{ Content = $voice; Pattern = 'xiaozhi_audio_pipeline_resume_after_playback\(\);'; Message = 'The voice state machine must reset AFE after playout ends.' },
    @{ Content = $startup; Pattern = 'audio_playback_service_enqueue_pcm'; Message = 'Startup PCM must use the same playout service as cloud TTS.' },
    @{ Content = $cmake; Pattern = 'MID/system/audio_playback_service\.c'; Message = 'The shared playout service must be compiled into the firmware.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

if ($pipeline -match 'i2s_audio_driver_play\(') {
    throw 'The voice codec task must not write PCM directly to I2S.'
}
if ($startup -match 'i2s_audio_driver_play\(') {
    throw 'Startup audio must not write PCM directly to I2S.'
}
$writePath = [regex]::Match($playout, 'bool frame_current = false;[\s\S]*?if \(err != ESP_OK\)').Value
if ($writePath -notmatch 'xSemaphoreTake\(s_playout\.io_lock, portMAX_DELAY\)' -or
    $writePath -notmatch 'xSemaphoreGive\(s_playout\.lock\);[\s\S]*i2s_audio_driver_play\(') {
    throw 'The blocking I2S write must be serialized separately from the playout state lock.'
}

$ttsStartBody = [regex]::Match($voice, 'if \(cJSON_IsString\(state\).*?s_state = XIAOZHI_VOICE_TTS_BUFFERING;.*?publish_ui_state\(VOICE_UI_THINKING\);', 'Singleline').Value
if ($ttsStartBody -match 'publish_ui_state\(VOICE_UI_SPEAKING\)') {
    throw 'Cloud tts start must not switch the UI to SPEAKING before PCM playout starts.'
}
if ($voice -notmatch 'PCM playout started:|cloud TTS audible:') {
    throw 'The first successful PCM write must be observable before SPEAKING is published.'
}

Write-Output 'Audio playout stability contract passed.'
