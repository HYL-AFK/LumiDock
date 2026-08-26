$ErrorActionPreference = 'Stop'

$manifest = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"
$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"
$watchdog = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\watchdog_service.c"

@(
    @{ Content = $manifest; Pattern = 'mos_driver\.c'; Message = 'The deferred external MOS driver must not be compiled.' },
    @{ Content = $manager; Pattern = 'mos_driver'; Message = 'Application startup must not initialize the deferred MOS driver.' },
    @{ Content = $watchdog; Pattern = 'mos_driver'; Message = 'The watchdog must not control an unconnected external MOS.' }
) | ForEach-Object {
    if ($_.Content -match $_.Pattern) { throw $_.Message }
}

Write-Output 'No external MOS driver contract passed.'

