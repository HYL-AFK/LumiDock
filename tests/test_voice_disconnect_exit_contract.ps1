$ErrorActionPreference = 'Stop'
$voice = Get-Content (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_voice_service.c') -Raw
$uplinkStart = $voice.IndexOf('static bool send_uplink_packet')
$uplink = $voice.Substring($uplinkStart, $voice.IndexOf('static bool send_listen_start') - $uplinkStart)
if ($uplink -notmatch 'voice turn transport failure') { throw 'Transport failure must be logged at the failing uplink operation.' }
if ($uplink -match 'request_cloud_exit_prompt') { throw 'Uplink failure must not trigger an exit prompt.' }
if ($voice -notmatch 'follow-up window expired; requesting generated exit prompt') { throw 'Completed idle sessions must retain the 15 second exit path.' }
if ($voice -notmatch 'exit prompt selected: local PCM bytes=') { throw 'Exit prompt selection must remain observable.' }
Write-Output 'Voice disconnect and exit contract passed.'
