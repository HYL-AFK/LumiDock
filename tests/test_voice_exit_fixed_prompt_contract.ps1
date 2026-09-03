$ErrorActionPreference = 'Stop'
$voice = Get-Content (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_voice_service.c') -Raw

foreach ($phrase in @(
    '那我们今天先到这里啦，下次有需要随时找我～祝你一切顺利；',
    '本次对话结束，期待下次再见，愿你今天过得愉快；')) {
    if ($voice.IndexOf($phrase) -lt 0) { throw "Missing fixed exit phrase: $phrase" }
}
if ($voice.IndexOf('先告一段落咯，有空再来聊聊，万事顺心；') -ge 0) {
    throw 'Cancelled exit phrase must not remain in the voice service.'
}
if ($voice -match '请自然地说一句简短的告别语') {
    throw 'Exit prompt must not ask the cloud to freely generate a farewell.'
}
foreach ($marker in @('exit_tts_start_deadline_us', 'exit_tts_first_packet_deadline_us', 'exit_tts_playback_deadline_us')) {
    if ($voice -notmatch $marker) { throw "Missing independent exit deadline: $marker" }
}
if ($voice -notmatch 'exit prompt PCM drained; closing voice session') {
    throw 'Exit session must close only after PCM drain.'
}
Write-Output 'Fixed exit prompt contract passed.'
