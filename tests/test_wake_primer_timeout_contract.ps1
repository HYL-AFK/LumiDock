$ErrorActionPreference = 'Stop'
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

if ($voice -notmatch 'XIAOZHI_WAKE_PRIMER_TIMEOUT_US\s*\(10LL\s*\*\s*1000LL\s*\*\s*1000LL\)') {
    throw 'Wake primer timeout must be bounded at 10 seconds.'
}

$start = $voice.IndexOf('if (s_waiting_for_wake_primer && s_wake_primer_deadline_us != 0')
$end = $voice.IndexOf('if (s_state == XIAOZHI_VOICE_WAIT_RESPONSE', $start)
if ($start -lt 0 -or $end -lt 0) { throw 'Wake primer timeout handling block is missing.' }
$block = $voice.Substring($start, $end - $start)
if ($block -match 'close_channel\(') { throw 'Wake primer timeout must not close the WSS channel.' }
if ($block -notmatch '获取数据超时|data timeout|timeout') { throw 'Wake primer timeout must log a data-timeout reason.' }
if ($block -notmatch 'XIAOZHI_VOICE_WAIT_FOLLOWUP|s_followup_deadline_us') {
    throw 'Wake primer timeout must keep the existing WSS session usable for a bounded follow-up.'
}
if ($block -match 'publish_ui_state\(VOICE_UI_ERROR\)') {
    throw 'Wake primer timeout must not turn an otherwise healthy wake session into a UI error.'
}

Write-Output 'Wake primer timeout contract passed.'
