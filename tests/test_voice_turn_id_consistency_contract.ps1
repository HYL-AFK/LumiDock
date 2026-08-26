$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

$startListenStart = $voice.IndexOf('static void start_listen')
$startListenEnd = $voice.IndexOf('static void finish_boot_greeting_failure', $startListenStart)
if ($startListenStart -lt 0 -or $startListenEnd -lt 0) { throw 'start_listen block is missing.' }
$startListenBlock = $voice.Substring($startListenStart, $startListenEnd - $startListenStart)

$turnIncrement = $startListenBlock.IndexOf('++s_turn_id')
$sendStart = $startListenBlock.IndexOf('send_listen_start()')
if ($turnIncrement -lt 0 -or $sendStart -lt 0 -or $turnIncrement -gt $sendStart) {
    throw 'start_listen must allocate the turn before listen/start so control and Opus logs share one turn id.'
}
if ($startListenBlock -notmatch 's_waiting_turn_id = 0U;' -or $startListenBlock -notmatch 's_playing_turn_id = 0U;') {
    throw 'A new listen turn must clear stale waiting/playing turn ownership.'
}

Write-Output 'Voice turn id consistency contract passed.'
