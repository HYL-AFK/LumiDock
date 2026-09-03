$ErrorActionPreference = 'Stop'
$driver = Get-Content -Raw "$PSScriptRoot\..\main\Dri\i2s_audio_driver.c"
$header = Get-Content -Raw "$PSScriptRoot\..\main\Dri\i2s_audio_driver.h"
$iot = Get-Content -Raw "$PSScriptRoot\..\main\MID\system\voice_iot_command_service.c"
$radio = Get-Content -Raw "$PSScriptRoot\..\main\MID\system\radio_stream_service.c"
$play = Get-Content -Raw "$PSScriptRoot\..\main\MID\system\audio_playback_service.h"
$cmake = Get-Content -Raw "$PSScriptRoot\..\main\CMakeLists.txt"
if ($header -notmatch 'i2s_audio_driver_set_output_volume') { throw 'missing volume API' }
if ($driver -notmatch 'esp_codec_dev_set_out_vol') { throw 'volume must use codec API' }
if ($iot -notmatch 'volume') { throw 'voice volume commands missing' }
if ($iot -notmatch 'radio') { throw 'voice radio commands missing' }
$iot_hex = [BitConverter]::ToString([IO.File]::ReadAllBytes((Join-Path $PSScriptRoot '..\main\MID\system\voice_iot_command_service.c')))
foreach ($phraseHex in @('E5-A4-A7-E9-9F-B3-E9-87-8F','E5-B0-8F-E9-9F-B3-E9-87-8F','E6-94-BE-E7-94-B5-E5-8F-B0')) {
    if (!$iot_hex.Contains($phraseHex)) { throw "missing measured command phrase bytes: $phraseHex" }
}
if ($radio -notmatch 'esp_http_client|esp_audio_simple_dec_process') { throw 'radio must use HTTP and MP3 decoder' }
if ($play -notmatch 'AUDIO_PLAYOUT_SOURCE_RADIO') { throw 'radio playback source missing' }
if ($cmake -notmatch 'radio_stream_service.c') { throw 'radio service not compiled' }
if ($radio -notmatch 'RADIO_MAX_RECONNECTS') { throw 'radio reconnect bound missing' }
if ($radio -notmatch 'audio_playback_service_abort') { throw 'radio failure cleanup missing' }
if ($radio -notmatch 'radio_stream_service_consume_failure') { throw 'radio failure status API missing' }
Write-Output 'Audio radio and volume contract passed.'
