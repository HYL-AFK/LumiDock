$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

if ($voice -match 'tail send failed; sending listen/stop fallback|wait_for_uplink_tail_flush|VOICE_EVENT_UPLINK_TAIL_FLUSHED') {
    throw 'STOPPING must not retain the old generic tail-flush or same-connection stop fallback race.'
}

$terminalStart = $voice.IndexOf('static bool send_terminal_uplink_sequence')
$finishStart = $voice.IndexOf('static void finish_listen_with_tail_flush')
if ($terminalStart -lt 0 -or $finishStart -lt 0) { throw 'Dedicated terminal uplink sender is missing.' }
$terminalBlock = $voice.Substring($terminalStart, $finishStart - $terminalStart)
if ($terminalBlock -notmatch '"bin-tail"' -or
    $terminalBlock -notmatch 's_uplink_tx_operation = "listen-stop"' -or
    $terminalBlock -notmatch 'send_listen_stop\(\)' -or
    $terminalBlock -notmatch 'close_channel\(false\)') {
    throw 'One terminal owner must send captured tails then listen/stop and close on either write failure.'
}

$sendPacketStart = $voice.IndexOf('static bool send_uplink_packet')
$sendPacketEnd = $voice.IndexOf('static bool send_listen_start', $sendPacketStart)
if ($sendPacketStart -lt 0 -or $sendPacketEnd -lt 0) { throw 'send_uplink_packet block is missing.' }
$sendPacketBlock = $voice.Substring($sendPacketStart, $sendPacketEnd - $sendPacketStart)
if ($sendPacketBlock -notmatch 'op=%s' -or $sendPacketBlock -notmatch 'terminal_gen=%u') {
    throw 'Opus write diagnostics must include its operation and terminal generation.'
}

Write-Output 'Voice terminal stop failure contract passed.'
