$ErrorActionPreference = 'Stop'

$root = Join-Path $PSScriptRoot '..'
$defaults = Get-Content -LiteralPath (Join-Path $root 'sdkconfig.defaults') -Raw
$resolved = Get-Content -LiteralPath (Join-Path $root 'sdkconfig') -Raw
$partitions = Get-Content -LiteralPath (Join-Path $root 'partitions.csv') -Raw

if ($defaults -notmatch 'CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y') {
    throw 'The default firmware image header must target the detected 8 MB Flash.'
}
if ($resolved -notmatch 'CONFIG_ESPTOOLPY_FLASHSIZE="8MB"') {
    throw 'The resolved firmware configuration must target 8 MB Flash.'
}
if ($partitions -notmatch '(?m)^factory,\s+app,\s+factory,\s+0x10000,\s+0x770000,' -or
    $partitions -notmatch '(?m)^model,\s+data,\s+spiffs,\s*,\s*0x80000,') {
    throw 'The 8 MB layout must allocate 0x770000 to the application and reserve the final 512 KiB for ESP-SR models.'
}

Write-Output '8 MB Flash layout contract passed.'
