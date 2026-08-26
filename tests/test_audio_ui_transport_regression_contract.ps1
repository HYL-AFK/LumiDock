$ErrorActionPreference = 'Stop'
$playout = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\audio_playback_service.c"
$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

if ($playout -notmatch 'AUDIO_PLAYOUT_LOG_INTERVAL_US') {
    throw 'PCM playout diagnostics must be rate limited so UART logging cannot starve UI work.'
}
if ($pipeline -notmatch 'TTS_DECODE_LOG_INTERVAL_US') {
    throw 'TTS decode diagnostics must be rate limited during a streaming response.'
}
if ($pipeline -notmatch 'STOP_STREAM:[\s\S]*last_vad_state\s*=\s*VAD_SILENCE') {
    throw 'Stopping a failed voice turn must restore the AFE silence baseline.'
}
if ($voice -notmatch 'voice turn transport failure[\s\S]*xiaozhi_audio_pipeline_reset_after_silence') {
    throw 'Transport failure must reset the AFE baseline before the next wake.'
}
Write-Output 'Audio UI and transport regression contract passed.'
