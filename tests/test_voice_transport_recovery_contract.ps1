$ErrorActionPreference = 'Stop'
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

if ($voice -notmatch 'send_result\s*>\s*0') {
    throw 'Only a positive WebSocket send result may be treated as success.'
}
if ($voice -notmatch 'voice turn transport failure') {
    throw 'Transport failure must identify the affected voice turn.'
}
# 一次失败的 Opus 上行必须终止本轮。链路已经被标记 FAILED，继续停在原地等云端
# 只会让设备看起来死机，所以走的是本地错误音收场而不是云端兜底——这里锁的是
# "失败 → 收场"这条因果，不再绑某一句话的措辞。
if ($voice -notmatch '(?s)voice turn transport failure; no cloud fallback, entering bounded cleanup[\s\S]{0,300}?play_error_tone_then_close') {
    throw 'A failed Opus packet must end the current turn instead of leaving the session parked.'
}
# 链路还活着、只是某一步没成的情况，仍然要请云端生成兜底回复。
if ($voice -notmatch 'requesting generated fallback prompt') {
    throw 'A recoverable send failure must still ask the cloud for a fallback prompt.'
}
if ($voice -notmatch 'discard_uplink_queue\(\)') {
    throw 'A failed turn must discard queued Opus packets before returning to wake mode.'
}
if ($voice -match 'esp_websocket_client_send_bin\([\s\S]*?\);\s*if \(send_result < 0\)') {
    throw 'The uplink path must not accept only negative send results as failure.'
}
Write-Output 'Voice transport recovery contract passed.'
