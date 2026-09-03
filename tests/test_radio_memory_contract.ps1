$ErrorActionPreference = 'Stop'
$radio = Get-Content -Raw "$PSScriptRoot\..\main\MID\system\radio_stream_service.c"
if ($radio -match 'xTaskCreate\(radio_task') { throw 'radio task must not consume internal stack via plain xTaskCreate' }
if ($radio -notmatch 'xTaskCreatePinnedToCoreWithCaps') { throw 'radio task must use capability-aware PSRAM stack creation' }
if ($radio -notmatch 'MALLOC_CAP_SPIRAM') { throw 'radio task must request PSRAM stack' }
Write-Output 'Radio memory contract passed.'
