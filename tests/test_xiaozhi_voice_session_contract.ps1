$ErrorActionPreference = 'Stop'

$partitions = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\partitions.csv"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$activation = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_activation_service.c"
$activationHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_activation_service.h"
$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$pipelineHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.h"
$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"

@(
    @{ Content = $partitions; Pattern = '(?m)^model,\s+data,\s+spiffs,\s*,\s*0x80000,'; Message = 'ESP-SR model partition must provide both a subtype and a size field.' },
    @{ Content = $voice; Pattern = 'static void voice_session_task\(void \*arg\)'; Message = 'A dedicated voice session task must own the WSS state machine.' },
    @{ Content = $voice; Pattern = 's_event_queue\s*=\s*xQueueCreate'; Message = 'WSS callbacks must hand control messages to a session event queue.' },
    @{ Content = $voice; Pattern = 's_uplink_queue\s*=\s*xRingbufferCreateWithCaps'; Message = 'Opus encoding must write to a PSRAM-backed uplink queue.' },
    @{ Content = $voice; Pattern = 'payload_offset'; Message = 'WebSocket payloads must be reassembled using payload offsets.' },
    @{ Content = $voice; Pattern = 'payload_len'; Message = 'WebSocket payloads must be bounded by their declared total length.' },
    @{ Content = $voice; Pattern = 'VOICE_EVENT_WSS_BINARY'; Message = 'Completed downlink audio frames must be delivered to the session task.' },
    @{ Content = $voice; Pattern = 'VOICE_EVENT_WAKE'; Message = 'WakeNet must notify the session task rather than operating WSS directly.' },
    @{ Content = $voice; Pattern = 'VOICE_EVENT_VAD_START'; Message = 'VAD start must notify the session task.' },
    @{ Content = $voice; Pattern = 'xiaozhi_audio_pipeline_abort_playback'; Message = 'Abort and disconnect must clear queued TTS audio.' },
    @{ Content = $pipelineHeader; Pattern = 'xiaozhi_audio_pipeline_abort_playback'; Message = 'Audio pipeline must expose a playback abort operation.' },
    @{ Content = $pipeline; Pattern = 'playback_epoch'; Message = 'Playback abort must invalidate queued audio with a generation marker.' },
    @{ Content = $pipeline; Pattern = 'voice playback queue discarded by codec'; Message = 'The codec task must discard queued TTS after an abort.' },
    @{ Content = $pipeline; Pattern = 'clear_reference_pending'; Message = 'The removed AEC reference path must not remain in the audio pipeline.'; Negate = $true },
    @{ Content = $activationHeader; Pattern = 'xiaozhi_activation_service_refresh_credentials'; Message = 'Voice failures must be able to request fresh WSS credentials.' },
    @{ Content = $activation; Pattern = 'static void activation_task\(void \*arg\)'; Message = 'OTA activation must run outside the snapshot/UI polling task.' },
    @{ Content = $activation; Pattern = 'xiaozhi_activation_service_refresh_credentials'; Message = 'Activation service must invalidate stale WSS credentials on request.' }
) | ForEach-Object {
    $matched = $_.Content -match $_.Pattern
    if (($_.Negate -and $matched) -or (-not $_.Negate -and -not $matched)) { throw $_.Message }
}

if ($manager -match 'xiaozhi_activation_service_poll\(\)') {
    throw 'Snapshot bridge must not synchronously perform cloud OTA registration.'
}

Write-Output 'XiaoZhi voice session contract passed.'

