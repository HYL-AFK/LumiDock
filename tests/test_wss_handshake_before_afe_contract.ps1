$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

@(
    @{ Pattern = 'XIAOZHI_PREWARM_READY_TIMEOUT_US'; Message = 'Voice startup must define a bounded WSS prewarm handshake timeout.' },
    @{ Pattern = 'static bool wait_for_prewarmed_wss\(void\)'; Message = 'Voice startup must wait for the WSS hello state before allocating AFE.' },
    @{ Pattern = 'WSS prewarm ready before AFE allocation'; Message = 'Voice startup must report a completed WSS hello before AFE allocation.' },
    @{ Pattern = 'WSS prewarm handshake timed out before AFE allocation'; Message = 'Voice startup must report a bounded WSS prewarm failure without rebooting.' }
) | ForEach-Object {
    if ($voice -notmatch $_.Pattern) { throw $_.Message }
}

$initStart = $voice.IndexOf('esp_err_t xiaozhi_voice_service_init(void)')
$init = $voice.Substring($initStart)
$prewarmPos = $init.IndexOf('prewarm_wss()')
$sessionTaskPos = $init.IndexOf('xTaskCreatePinnedToCoreWithCaps(voice_session_task')
$waitPos = $init.IndexOf('wait_for_prewarmed_wss()')
$uplinkTaskPos = $init.IndexOf('xTaskCreatePinnedToCoreWithCaps(voice_uplink_tx_task')
$afePos = $init.IndexOf('init_audio_pipeline_on_internal_stack()')
if ($prewarmPos -lt 0 -or $sessionTaskPos -lt 0 -or $waitPos -lt 0 -or
    $uplinkTaskPos -lt 0 -or $afePos -lt 0 -or
    -not ($prewarmPos -lt $sessionTaskPos -and $sessionTaskPos -lt $waitPos -and
          $waitPos -lt $uplinkTaskPos -and $uplinkTaskPos -lt $afePos)) {
    throw 'WSS prewarm must precede the session task; hello must precede the uplink task and AFE initialization.'
}

Write-Output 'WSS handshake-before-AFE contract passed.'
