$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

@(
    @{ Pattern = 'cloud JSON received: %s'; Message = 'Inbound cloud text frames must log their raw JSON payload for COM6 diagnosis.' },
    @{ Pattern = 'cloud JSON processed: type=%s state=%s'; Message = 'Inbound cloud text frames must log their parsed type and state after processing.' },
    @{ Pattern = 'cJSON_GetObjectItemCaseSensitive\(root, "state"\)'; Message = 'Processed JSON logging must read the optional state field safely.' }
) | ForEach-Object {
    if ($voice -notmatch $_.Pattern) { throw $_.Message }
}

Write-Output 'Voice JSON logging contract passed.'
