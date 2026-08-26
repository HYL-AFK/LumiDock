$ErrorActionPreference = 'Stop'

$pins = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\board_pins.h"
$gate = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\startup_gate.c"
$main = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\main.c"

@(
    @{ Pattern = '#define LUMIDOCK_BUTTON_GPIO GPIO_NUM_12'; Message = 'The confirmed S3-Zero button must use GPIO12.' },
    @{ Pattern = '#define LUMIDOCK_ES8311_I2C_SDA_GPIO GPIO_NUM_17'; Message = 'ES8311 I2C SDA must use GPIO17.' },
    @{ Pattern = '#define LUMIDOCK_ES8311_I2C_SCL_GPIO GPIO_NUM_18'; Message = 'ES8311 I2C SCL must use GPIO18.' },
    @{ Pattern = '#define LUMIDOCK_I2S_MCLK_GPIO GPIO_NUM_38'; Message = 'ES8311 MCLK must use GPIO38.' }
) | ForEach-Object {
    if ($pins -notmatch $_.Pattern) { throw $_.Message }
}

if ($gate -match 'GPIO14') {
    throw 'Startup gate messages and comments must not retain the old GPIO14 button assignment.'
}
if ($main -match 'GPIO14') {
    throw 'Main must not report the retired GPIO14 startup gate.'
}

if ($pins -match 'LUMIDOCK_MOS[12]_GPIO') {
    throw 'The deferred external MOS GPIO assignments must not remain in the board contract.'
}

Write-Output 'ES8311 and button board-pin contract passed.'

