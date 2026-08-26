$ErrorActionPreference = 'Stop'

$weather = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_weather.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"
$led = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\status_led_service.c"

@(
    @{ Content = $weather; Pattern = 'xTaskCreatePinnedToCore\(weather_task, "weather_task", 6144, NULL, 3,\s*&s_weather_task_handle, 0\)'; Message = 'The weather task must keep an internal stack because persistent event logging uses NVS and temporarily disables cache.' },
    @{ Content = $weather; Pattern = 'xTaskCreatePinnedToCoreWithCaps\(weather_task'; Message = 'The weather task must never use a PSRAM stack while it can write NVS.'; Negate = $true },
    @{ Content = $manager; Pattern = '#include "freertos/idf_additions.h"'; Message = 'The snapshot bridge must include the capability-aware FreeRTOS task API.' },
    @{ Content = $manager; Pattern = 'xTaskCreatePinnedToCoreWithCaps\(snapshot_bridge_task, "snapshot_bridge", 3072, NULL, 2, NULL, 1,\s*MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT\)'; Message = 'The pure snapshot bridge must use a PSRAM stack to reserve internal memory for WSS.' },
    @{ Content = $led; Pattern = '#include "freertos/idf_additions.h"'; Message = 'The LED service must include the capability-aware FreeRTOS task API.' },
    @{ Content = $led; Pattern = 'xTaskCreatePinnedToCoreWithCaps\(status_led_task, "status_led", 3072, NULL, 1, NULL, 1,\s*MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT\)'; Message = 'The WS2812 status renderer must use a PSRAM stack to reserve internal memory for WSS.' },
    @{ Content = $voice; Pattern = '\.task_stack = 4096'; Message = 'WSS must use the ESP-WebSocket client default 4096-byte task stack.' },
    @{ Content = $voice; Pattern = 'WSS start memory: stack=%u internal=%u largest=%u'; Message = 'WSS startup must log the available internal contiguous block before task creation.' }
) | ForEach-Object {
    $matched = $_.Content -match $_.Pattern
    if (($_.Negate -and $matched) -or (-not $_.Negate -and -not $matched)) { throw $_.Message }
}

Write-Output 'WebSocket task-memory contract passed.'
