$ErrorActionPreference = 'Stop'
$playout = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\audio_playback_service.c"
$driver = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\i2s_audio_driver.c"
$driverHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\i2s_audio_driver.h"

if ($playout -notmatch 'playout_silence|silence_frame|underrun.*silence') {
    throw 'A playback underrun must submit silence instead of toggling the DAC mute state.'
}
if ($playout -match 'mute_output_serialized\(\);[\s\S]{0,500}AUDIO_PLAYOUT_EVENT_UNDERRUN') {
    throw 'The underrun path must not mute and later unmute the output.'
}
if ($playout -match 's_playout\.started\s*=\s*false;[\s\S]{0,500}TTS playout underrun') {
    throw 'TTS underrun must keep the playout stream started so zero PCM continues during a short gap.'
}
if ($playout -match 'source\s*==\s*AUDIO_PLAYOUT_SOURCE_STARTUP[\s\S]{0,500}s_playout\.started\s*=\s*false') {
    throw 'Startup underrun must not reset the started state while the DAC remains enabled.'
}
if ($driver -match 'i2s_audio_driver_play[\s\S]{0,1200}set_output_muted\(false\)') {
    throw 'The low-level PCM write must not implicitly toggle DAC mute state.'
}
if ($driver -notmatch 's_codec_lock|codec_lock') {
    throw 'All ES8311 codec operations need a driver-level serialization lock.'
}
if ($driverHeader -notmatch 'i2s_audio_driver_start_output|i2s_audio_driver_stop_output') {
    throw 'Playback ownership must expose explicit start/stop output operations.'
}
Write-Output 'Audio playout no-toggle underrun contract passed.'
