$ErrorActionPreference = 'Stop'
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$voiceBytes = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_voice_service.c'))

@(
    @{ Pattern = 'XIAOZHI_FOLLOWUP_WINDOW_US\s*\(15LL\s*\*\s*1000LL\s*\*\s*1000LL\)'; Message = 'Normal follow-up window must be fifteen seconds.' },
    @{ Pattern = 'XIAOZHI_BOOT_GREETING_REPLY_WINDOW_US\s*\(15LL\s*\*\s*1000LL\s*\*\s*1000LL\)'; Message = 'Boot greeting follow-up window must be fifteen seconds.' },
    @{ Pattern = 'XIAOZHI_VOICE_EXITING'; Message = 'Voice service needs a dedicated exit state.' },
    @{ Pattern = 'request_cloud_exit_prompt'; Message = 'Timeout exit must request a fixed farewell.' },
    @{ Pattern = 'exit prompt selected'; Message = 'Exit prompt selection must be logged.' },
    @{ Pattern = 'exit prompt PCM drained; closing voice session'; Message = 'Voice session must close only after the farewell audio drains.' }
) | ForEach-Object { if ($voice -notmatch $_.Pattern) { throw $_.Message } }
if ($voice -match '请自然地说一句简短的告别语') { throw 'Exit request must not ask for free-form generation.' }
if ($voice -match 'follow-up window expired; wake word required') {
    throw 'Follow-up expiry must no longer silently exit.'
}
Write-Output 'Generated exit contract passed.'
