$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"

@(
    @{ Pattern = 'config->vad_mode = VAD_MODE_0;'; Message = 'Low-level ES8311 microphone input requires ESP-SR normal VAD sensitivity for follow-up speech.' },
    @{ Pattern = 'config->vad_min_speech_ms = 128;'; Message = 'VAD must still require a brief sustained speech interval to avoid noise-triggered uplink.' },
    @{ Pattern = 'config->vad_min_noise_ms = 1000;'; Message = 'VAD must retain the one-second end-of-speech hold.' },
    @{ Pattern = 'xRingbufferSendAcquire\(s_pipeline\.tts_queue, \(void \*\*\)&packet, packet_bytes, 0\)'; Message = 'PSRAM TTS packet acquisition must use the ESP-IDF output-pointer API.' }
) | ForEach-Object {
    if ($pipeline -notmatch $_.Pattern) { throw $_.Message }
}

Write-Output 'Voice VAD sensitivity and TTS queue API contract passed.'
