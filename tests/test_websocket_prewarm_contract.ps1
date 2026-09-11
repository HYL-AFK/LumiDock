$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

@(
    @{ Pattern = 'static esp_err_t start_wss\(bool foreground_session\)'; Message = 'Voice service must expose a WSS startup helper that separates socket setup from a foreground conversation.' },
    @{ Pattern = 'static void prewarm_wss\(void\)'; Message = 'Voice service must prewarm the WebSocket before a WakeNet event.' },
    @{ Pattern = 'prewarm_wss\(\);'; Message = 'Voice initialization must prewarm the WebSocket while internal RAM is still contiguous.' },
    @{ Pattern = 'if \(s_pending_wake\) \{[\s\S]*?send_detect\(false\);[\s\S]*?s_pending_wake = false;'; Message = 'Cloud hello must only start detection for a real pending wake word without streaming the wake-word tail.' },
    @{ Pattern = 'start_wss\(true\)'; Message = 'A wake event must promote or create a foreground voice session.' },
    @{ Pattern = 'start_wss\(false\)'; Message = 'Prewarm must not activate the voice UI or listening state.' },
    @{ Pattern = 'if \(s_initialized && xiaozhi_activation_service_is_activated\(\)\)[\s\S]{0,600}?s_prewarm_retry_us = esp_timer_get_time\(\) \+ prewarm_delay_us;'; Message = 'A disconnected voice socket must retry in the background without refreshing credentials.' },
    @{ Pattern = 'XIAOZHI_PREWARM_RADIO_DELAY_US'; Message = 'Radio playback must keep a hot WebSocket instead of paying a cold TLS handshake on every wake.' },
    @{ Pattern = 'release_radio_after_close \|\| radio_stream_service_is_requested\(\)'; Message = 'A radio start deferred until after the voice chain closes must prewarm immediately too.' },
    @{ Pattern = 'const bool prewarm_radio_on = radio_stream_service_is_requested\(\);'; Message = 'The idle-wake prewarm schedule must also be radio aware.' }
) | ForEach-Object {
    if ($voice -notmatch $_.Pattern) { throw $_.Message }
}

Write-Output 'WebSocket prewarm contract passed.'
