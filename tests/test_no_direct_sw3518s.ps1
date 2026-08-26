$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$cmake = Get-Content -Raw (Join-Path $root "main\CMakeLists.txt")
$pins = Get-Content -Raw (Join-Path $root "main\Dri\board_pins.h")

if ($cmake -match '(?i)sw3518s_i2c|charging_service\.c') {
    throw "ESP32 must not compile the retired direct SW3518S driver or polling service."
}

if ($pins -match '(?i)SW3518S|LUMIDOCK_I2C_(PORT|SDA_GPIO|SCL_GPIO)') {
    throw "ESP32 board pins must not retain the retired direct SW3518S I2C contract."
}

if (-not (Test-Path (Join-Path $root "main\MID\mid_snapshot.h"))) {
    throw "Charging snapshot contract must remain for STC-provided data."
}

Write-Output "PASS: no direct SW3518S implementation remains on ESP32"

