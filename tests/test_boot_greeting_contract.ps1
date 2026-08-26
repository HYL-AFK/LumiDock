$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$header = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.h"

@(
    @{ Content = $header; Pattern = '#include <stdbool.h>'; Message = 'The public voice header must include stdbool before declaring a bool-returning API.' },
    @{ Content = $header; Pattern = 'xiaozhi_voice_service_start_boot_greeting'; Message = 'Boot greeting needs a public coordinator API.' },
    @{ Content = $voice; Pattern = 'XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS'; Message = 'Boot greeting must have an isolated TTS-wait state.' },
    @{ Content = $voice; Pattern = 'XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY'; Message = 'Boot greeting must have an isolated direct-reply state.' },
    @{ Content = $voice; Pattern = 'XIAOZHI_BOOT_GREETING_TTS_TIMEOUT_US \(8LL \* 1000LL \* 1000LL\)'; Message = 'Boot greeting must time out if TTS does not begin within eight seconds.' },
    @{ Content = $voice; Pattern = 'XIAOZHI_BOOT_GREETING_REPLY_WINDOW_US \(180LL \* 1000LL \* 1000LL\)'; Message = 'Boot greeting direct-reply window must last three minutes.' },
    @{ Content = $voice; Pattern = 's_boot_greeting_attempted'; Message = 'Boot greeting must be limited to once per power cycle.' },
    @{ Content = $voice; Pattern = 's_boot_greeting_request_pending'; Message = 'Repeated coordinator polls must not enqueue duplicate boot greeting events.' },
    @{ Content = $voice; Pattern = 's_pipeline_ready'; Message = 'Boot greeting must wait until Opus and AFE/WakeNet/VAD initialization is complete.' },
    @{ Content = $voice; Pattern = 's_boot_greeting_id'; Message = 'Boot greeting needs a distinct local round ID.' },
    @{ Content = $voice; Pattern = 'VOICE_EVENT_BOOT_GREETING'; Message = 'Coordinator must enter the voice state machine through an event.' },
    @{ Content = $voice; Pattern = 'if \(post_event\(VOICE_EVENT_BOOT_GREETING\)\) return ESP_OK;'; Message = 'A full state-machine queue must reject the boot greeting request instead of silently losing it.' },
    @{ Content = $voice; Pattern = 'boot greeting TTS timeout'; Message = 'Boot greeting timeout must be visible in COM6 logs.' },
    @{ Content = $voice; Pattern = 'boot greeting reply window expired; wake word required'; Message = 'Three-minute expiry must restore ordinary WakeNet.' },
    @{ Content = $voice; Pattern = 'xiaozhi_audio_pipeline_arm_followup\(\);'; Message = 'The existing five-second follow-up path must remain available after a user reply.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

Write-Output 'Boot greeting contract passed.'
