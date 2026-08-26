$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$codecStart = $pipeline.IndexOf('static void voice_codec_task(void *arg)')
$codecEnd = $pipeline.IndexOf('static esp_err_t init_afe', $codecStart)
if ($codecStart -lt 0 -or $codecEnd -lt 0) {
    throw 'The voice codec task boundary is missing.'
}
$codecTask = $pipeline.Substring($codecStart, $codecEnd - $codecStart)

if ($codecTask -match 'pcm_frame_t frame;') {
    throw 'The 2 KiB PCM queue item must not live on the voice codec task stack during Opus encoding.'
}
if ($codecTask -notmatch 's_pipeline\.codec_frame = heap_caps_malloc\(sizeof\(\*s_pipeline\.codec_frame\),[\s\S]*?MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT\)' -or
    $codecTask -notmatch 'xQueueReceive\(s_pipeline\.pcm_frames, s_pipeline\.codec_frame') {
    throw 'The voice codec task must receive its PCM queue item through a PSRAM buffer.'
}
if ($pipeline -notmatch '#define VOICE_CODEC_STACK_BYTES \(32U \* 1024U\)' -or
    $pipeline -notmatch 'voice_codec", VOICE_CODEC_STACK_BYTES') {
    throw 'The codec task must reserve the measured 32 KiB PSRAM stack required by esp_opus_enc_process().'
}

Write-Output 'Voice codec stack contract passed.'
