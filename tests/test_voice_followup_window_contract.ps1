$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$header = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.h"

@(
    @{ Content = $voice; Pattern = 'XIAOZHI_FOLLOWUP_WINDOW_US \(15LL \* 1000LL \* 1000LL\)'; Message = 'The post-TTS follow-up window must be fifteen seconds.' },
    @{ Content = $voice; Pattern = 'XIAOZHI_VOICE_WAIT_FOLLOWUP'; Message = 'Voice state machine must distinguish a follow-up window from normal wake-word idle.' },
    @{ Content = $voice; Pattern = 'xiaozhi_audio_pipeline_arm_followup\(\);'; Message = 'TTS completion must arm local VAD for a follow-up question.' },
    @{ Content = $voice; Pattern = 's_followup_deadline_us = esp_timer_get_time\(\) \+ XIAOZHI_FOLLOWUP_WINDOW_US;'; Message = 'TTS completion must set a bounded follow-up deadline.' },
    @{ Content = $voice; Pattern = 'if \(s_state == XIAOZHI_VOICE_WAIT_FOLLOWUP\) \{\s*if \(s_wait_for_turn_silence\) \{[\s\S]*?break;\s*\}\s*start_followup_listen\(\);'; Message = 'After residual TTS audio reaches silence, speech inside the follow-up window must initiate direct cloud streaming without a wake word.' },
    @{ Content = $voice; Pattern = 'follow-up window expired; requesting generated exit prompt'; Message = 'Follow-up timeout must request the cloud-generated farewell.' },
    @{ Content = $pipeline; Pattern = 'void xiaozhi_audio_pipeline_arm_followup\(void\)'; Message = 'Audio pipeline must expose a safe VAD-only follow-up arm function.' },
    @{ Content = $header; Pattern = 'void xiaozhi_audio_pipeline_arm_followup\(void\);'; Message = 'Follow-up arm function must be declared for the voice state machine.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

Write-Output 'Voice follow-up window contract passed.'
