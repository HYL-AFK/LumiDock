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
    @{ Content = $playout; Pattern = '#define AUDIO_PLAYOUT_TTS_PREBUFFER_FRAMES 50U'; Message = 'Cloud TTS must open with a 1.5 s prebuffer: the cloud supplies at real time, so the level at start is the only jitter buffer there is.' },
    @{ Content = $playout; Pattern = '#define AUDIO_PLAYOUT_QUEUE_DEPTH 256U'; Message = 'PCM playout needs a deep queue (256 frames ≈ 7.7 s) to absorb network burst/stutter.' },
    @{ Content = $playout; Pattern = '#define AUDIO_PLAYOUT_PREBUFFER_MAX_WAIT_US'; Message = 'The prebuffer wait must be time bounded: a short cloud sentence can never reach the target and would stay silent forever.' },
    @{ Content = $playout; Pattern = 'prebuffer_expired'; Message = 'The playout task must start with the frames it already has once the prebuffer wait expires.' },
    # 攒帧窗口必须从“第一帧入队”起算，不能从 begin() 起算：云端从收到 tts start 到
    # 发出第一个包实测要 880 ms，那段是云端延迟，算进来会把真正攒帧的时间砍掉一半，
    # 门槛也就永远够不到（实测日志 turn=11 只用 18/32 帧就开播，510 ms 后 underrun）。
    @{ Content = $playout; Pattern = 'first_frame_us != 0'; Message = 'The prebuffer window must start at the first queued frame, not at begin().' },
    @{ Content = $playout; Pattern = 's_playout\.first_frame_us = esp_timer_get_time\(\);'; Message = 'The first queued frame must stamp its own arrival time.' },
    @{ Content = $playout; Pattern = 's_playout\.first_frame_us = 0;'; Message = 'Each new playout session must clear the first-frame stamp, or the second turn inherits the previous one.' },
    # 两个门槛常量必须同步，否则开播判定和管线里的日志/超时对不上。
    @{ Content = $pipeline; Pattern = '#define XIAOZHI_TTS_PREBUFFER_FRAMES 50U'; Message = 'Both prebuffer constants must move together.' },
    # 停顿看门狗必须盖住实测到的 12.1~12.8 s 句间停顿，否则第二句会被 late packet 丢掉。
    @{ Content = $voice; Pattern = '#define XIAOZHI_TTS_STALL_TIMEOUT_US \(15LL'; Message = 'The stall watchdog must outlast the measured 12.8 s inter-sentence gap, or the next sentence is discarded as a late packet.' },
    @{ Content = $voice; Pattern = '#define XIAOZHI_TTS_FIRST_PCM_TIMEOUT_US'; Message = 'An accepted TTS turn that never reaches the speaker must time out instead of hanging.' },
    @{ Content = $voice; Pattern = 'TTS accepted but never became audible'; Message = 'The never-audible watchdog must finish playback locally so the turn recovers.' },
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
