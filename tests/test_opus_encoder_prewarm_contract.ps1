$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"

@(
    @{ Content = $pipeline; Pattern = 'esp_err_t xiaozhi_audio_pipeline_prepare_encoder\(void\)'; Message = 'The audio pipeline must expose an early Opus encoder preparation step.' },
    @{ Content = $pipeline; Pattern = 'Opus encoder prepared: internal_free=%u largest=%u'; Message = 'Encoder preparation must report remaining internal heap for diagnosing startup ordering.' },
    @{ Content = $voice; Pattern = 'xiaozhi_audio_pipeline_prepare_encoder\(\)'; Message = 'Voice initialization must prepare Opus before WSS and AFE startup.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

$init_start = $voice.IndexOf('esp_err_t xiaozhi_voice_service_init(void)')
$init = $voice.Substring($init_start)
$encoder_pos = $init.IndexOf('xiaozhi_audio_pipeline_prepare_encoder()')
$wss_pos = $init.IndexOf('prewarm_wss()')
$afe_pos = $init.IndexOf('init_audio_pipeline_on_internal_stack()')
if ($encoder_pos -lt 0 -or $wss_pos -lt 0 -or $afe_pos -lt 0 -or
    $encoder_pos -ge $wss_pos -or $wss_pos -ge $afe_pos) {
    throw 'The resource order must be Opus encoder, WSS, then AFE/WakeNet.'
}

Write-Output 'Opus encoder prewarm contract passed.'
