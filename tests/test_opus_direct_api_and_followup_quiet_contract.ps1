$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

@(
    @{ Content = $pipeline; Pattern = 'esp_opus_enc_process\(s_pipeline\.encoder, &input, &output\)'; Message = 'A direct Opus handle must use esp_opus_enc_process.' },
    @{ Content = $pipeline; Pattern = 'output\.encoded_bytes = 0U;'; Message = 'Each Opus call must clear the previous output length.' },
    @{ Content = $pipeline; Pattern = 'void xiaozhi_audio_pipeline_finish_playback'; Message = 'TTS completion must close input before hardware mute.' },
    @{ Content = $voice; Pattern = 'XIAOZHI_VOICE_TTS_DRAINING'; Message = 'The voice state machine must wait for PCM drain before follow-up.' },
    @{ Content = $voice; Pattern = 'xiaozhi_audio_pipeline_resume_after_playback\(\);'; Message = 'TTS drain must establish a fresh AFE baseline.' },
    @{ Content = $voice; Pattern = 's_state == XIAOZHI_VOICE_TTS_BUFFERING'; Message = 'VAD must not start a new turn while TTS is still buffering.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

if ($voice -match 's_wait_for_turn_silence = s_vad_speech') {
    throw 'Follow-up must not wait for a stale VAD end event after PCM drain.'
}

if ($pipeline -match 'esp_audio_enc_process\(s_pipeline\.encoder') {
    throw 'A direct Opus encoder handle must never pass through esp_audio_enc_process.'
}

Write-Output 'Direct Opus API and quiet follow-up contract passed.'
