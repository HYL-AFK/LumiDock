$ErrorActionPreference = 'Stop'

$wifi = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\wifi_manager.c"
$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$snapshot = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\mid_snapshot.h"
$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"

@(
    @{ Content = $wifi; Pattern = '#define INTERNET_PROBE_URL "https?://www\.qq\.com/favicon\.ico"'; Message = 'Public network probing must use the confirmed Tencent icon URL.' },
    @{ Content = $wifi; Pattern = 'internet probe failed: err=%s status=%d body=%u content_type=%d'; Message = 'Failed internet probes must log transport, HTTP, body and content-type evidence.' },
    @{ Content = $manager; Pattern = 'audio_startup_task'; Message = 'Audio startup must be owned by the pre-reserved audio task.' },
    @{ Content = $manager; Pattern = 'snapshot\.voice_activation\.activated'; Message = 'The pre-reserved audio task must wait for confirmed XiaoZhi activation.' },
    @{ Content = $manager; Pattern = '!snapshot\.voice_activation\.restart_required'; Message = 'Audio startup must not continue while activation requires a restart.' },
    @{ Content = $manager; Pattern = 'activation confirmed; starting deferred audio services'; Message = 'Deferred audio startup must be auditable on COM6.' },
    @{ Content = $snapshot; Pattern = 'bool active;'; Message = 'Voice snapshots must distinguish an actual wake-driven session from an idle service.' },
    @{ Content = $voice; Pattern = 'voice network lost ignored: no active session'; Message = 'Network loss while idle must not create a voice error page.' },
    @{ Content = $voice; Pattern = 's_session_active = true;'; Message = 'Only a wake-driven channel start may mark the voice UI active.' },
    @{ Content = $ui; Pattern = 'snapshot->voice_session\.active &&'; Message = 'The UI must require a wake-driven active session before showing the voice page.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

if ($manager -match 'voice_activation_gate_task') {
    throw 'The removed late activation-gate task must not reintroduce a second internal-RAM allocation.'
}

Write-Output 'Activation and voice gate contract passed.'
