$ErrorActionPreference = 'Stop'
$driver = Get-Content -Raw "$PSScriptRoot\..\main\Dri\i2s_audio_driver.c"
$pipeline = Get-Content -Raw "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$voice = Get-Content -Raw "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$iot = Get-Content -Raw "$PSScriptRoot\..\main\MID\system\voice_iot_command_service.c"
$iotBytes = [BitConverter]::ToString([IO.File]::ReadAllBytes((Join-Path $PSScriptRoot '..\main\MID\system\voice_iot_command_service.c')))
if ($driver -notmatch 'ES8311_MIC_INPUT_GAIN_DB 24\.0f') { throw 'microphone gain must use the 24 dB far-field pickup value' }
foreach ($pattern in @('capture_rms','capture_peak','capture_frames','capture_bytes','capture_cache_bytes','capture_duration_us','capture_short','capture_low_level')) {
    if ($pipeline -notmatch $pattern -and $voice -notmatch $pattern) { throw "missing per-turn capture diagnostic: $pattern" }
}
if (!$iotBytes.Contains('E4-B8-8D') -or $iot -notmatch 'negation') { throw 'negative volume STT must be rejected before substring matching' }
if ($voice -notmatch 'XIAOZHI_UPLINK_MAIN_WRITE_WAIT_TIMEOUT_US' -or $voice -notmatch 'in-flight bin-main completed') { throw 'terminal path must wait for in-flight bin-main' }
Write-Output 'Voice capture accuracy contract passed.'
