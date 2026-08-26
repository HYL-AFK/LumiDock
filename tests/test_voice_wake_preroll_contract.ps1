$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$start = $pipeline.IndexOf('case VOICE_AUDIO_CONTROL_START_STREAM:')
$stop = $pipeline.IndexOf('case VOICE_AUDIO_CONTROL_STOP_STREAM:', $start)
if ($start -lt 0 -or $stop -lt 0) {
    throw 'The AFE stream-control boundaries are missing.'
}

$startControl = $pipeline.Substring($start, $stop - $start)
if ($startControl -notmatch 's_pipeline\.afe->reset_buffer\(s_pipeline\.afe_data\)') {
    throw 'Starting cloud uplink must clear the already-detected wake-word tail from the AFE owner task.'
}
if ($startControl -notmatch 's_pipeline\.pcm_partial_len = 0U;' -or
    $startControl -notmatch 'discard_pcm_frames\(\);') {
    throw 'Starting cloud uplink must clear both partial and queued pre-roll PCM.'
}

Write-Output 'Wake-word pre-roll isolation contract passed.'
