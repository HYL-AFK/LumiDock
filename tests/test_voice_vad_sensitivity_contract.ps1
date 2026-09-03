$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"

@(
    @{ Pattern = 'config->vad_mode = VAD_MODE_0;'; Message = 'Low-level ES8311 microphone input requires ESP-SR normal VAD sensitivity for follow-up speech.' },
    @{ Pattern = 'config->vad_min_speech_ms = 128;'; Message = 'VAD must still require a brief sustained speech interval to avoid noise-triggered uplink.' },
    @{ Pattern = 'config->vad_min_noise_ms = 1500;'; Message = 'VAD must retain the 1.5-second end-of-speech hold for natural phrase tails.' },
    @{ Pattern = 'xRingbufferSendAcquire\(s_pipeline\.tts_queue, \(void \*\*\)&packet, packet_bytes, 0\)'; Message = 'PSRAM TTS packet acquisition must use the ESP-IDF output-pointer API.' },
    @{ Pattern = '#define VAD_START_MIN_RECENT_PEAK 800U'; Message = 'VAD starts must be gated by a recent-peak energy floor above the measured noise floor.' },
    @{ Pattern = 'VAD start gated by energy: recent_peak=%u threshold=%u'; Message = 'Gated noise VAD starts must be logged with the recent peak and threshold.' },
    @{ Pattern = 'uint32_t vad_peak_window\[VAD_RECENT_PEAK_SAMPLES\];'; Message = 'The energy gate must keep a sliding window of recent feed peaks.' },
    @{ Pattern = 'if \(vad_speech && recent_peak < VAD_START_MIN_RECENT_PEAK\) \{[\s\S]*?continue;'; Message = 'A sub-threshold VAD start must be rejected without updating last_vad_state.' }
) | ForEach-Object {
    if ($pipeline -notmatch $_.Pattern) { throw $_.Message }
}

Write-Output 'Voice VAD sensitivity, energy gate, and TTS queue API contract passed.'
