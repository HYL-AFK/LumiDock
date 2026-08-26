$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$partitions = Get-Content (Join-Path $root 'partitions.csv') -Raw
$cmake = Get-Content (Join-Path $root 'main/CMakeLists.txt') -Raw
$pins = Get-Content (Join-Path $root 'main/Dri/board_pins.h') -Raw

if ($partitions -match '(?m)^ui_assets,') {
    throw 'ui_assets partition must not reserve flash when UI assets are embedded in the application.'
}

if ($partitions -notmatch '(?m)^factory,\s+app,\s+factory,\s+0x10000,\s+0x770000,') {
    throw 'factory application partition must cover the 8 MB layout before the model partition.'
}

if ($cmake -match 'single_wire_uart_service\.c') {
    throw 'single_wire_uart_service must not be compiled while it has no implementation.'
}

if ($pins -match 'LUMIDOCK_SINGLE_UART_GPIO') {
    throw 'obsolete single-wire UART GPIO definition must be removed.'
}

if (Test-Path (Join-Path $root 'main/MID/single_wire_uart_service.c')) {
    throw 'obsolete single-wire UART source must be removed.'
}

if (Test-Path (Join-Path $root 'main/MID/single_wire_uart_service.h')) {
    throw 'obsolete single-wire UART header must be removed.'
}

Write-Output 'flash space cleanup configuration is valid'

