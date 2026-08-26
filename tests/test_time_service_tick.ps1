$ErrorActionPreference = 'Stop'
$service = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\network\time_service.c"

if ($service -notmatch '#include <time.h>') {
    throw 'Time service must include the system clock API.'
}
if ($service -notmatch 'const time_t now = time\(NULL\);') {
    throw 'Time service must read the running system clock on every poll.'
}
if ($service -notmatch 'localtime_r\(&now, &value.local_time\);') {
    throw 'Time service must publish the current local time on every poll.'
}
if ($service -match 'value\.local_time = source\.local_time;') {
    throw 'Time service must not repeatedly publish the frozen SNTP completion snapshot.'
}

Write-Output 'Time service tick checks passed.'

