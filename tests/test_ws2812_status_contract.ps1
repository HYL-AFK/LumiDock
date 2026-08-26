$ErrorActionPreference = 'Stop'

$driver = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\ws2812_driver.c"
$driverHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\ws2812_driver.h"
$boardPins = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\board_pins.h"
$service = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\status_led_service.c"
$serviceHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\status_led_service.h"
$snapshot = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\mid_snapshot.h"
$wifi = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\network\wifi_service.c"
$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"

@(
    @{ Content = $boardPins; Pattern = 'LUMIDOCK_WS2812_LED_COUNT 24U'; Message = 'The status strip must be configured for 24 WS2812 LEDs.' },
    @{ Content = $driver; Pattern = 'rmt_new_tx_channel'; Message = 'WS2812 output must use the RMT TX peripheral, not a GPIO level.' },
    @{ Content = $driver; Pattern = 'rmt_new_simple_encoder'; Message = 'WS2812 output must provide an encoder for GRB bit timing.' },
    @{ Content = $driver; Pattern = 'rmt_transmit'; Message = 'WS2812 colors must be transmitted through RMT.' },
    @{ Content = $driver; Pattern = 'rmt_tx_wait_all_done\(s_channel, portMAX_DELAY\)'; Message = 'The initial WS2812 frame must wait for RMT completion instead of failing after a short timeout.' },
    @{ Content = $driver; Pattern = 'const size_t byte_index = symbols_written / 8U;'; Message = 'The RMT encoder must convert already written symbols into a WS2812 data-byte offset.' },
    @{ Content = $driver; Pattern = 'ws2812_driver_set_rgb'; Message = 'The driver must expose direct RGB output.' },
    @{ Content = $driver; Pattern = 'ws2812_driver_set_pixel'; Message = 'The driver must support individual pixels for strip animations.' },
    @{ Content = $driver; Pattern = 'ws2812_driver_show'; Message = 'The driver must transmit a complete strip frame.' },
    @{ Content = $driverHeader; Pattern = 'ws2812_driver_set_rgb'; Message = 'The RGB output API must be public.' },
    @{ Content = $driverHeader; Pattern = 'ws2812_driver_set_pixel'; Message = 'The pixel output API must be public.' },
    @{ Content = $driverHeader; Pattern = 'ws2812_driver_show'; Message = 'The strip flush API must be public.' },
    @{ Content = $serviceHeader; Pattern = 'status_led_service_init'; Message = 'The application needs one status LED service.' },
    @{ Content = $service; Pattern = 'STATUS_LED_UPDATE_MS 50U'; Message = 'Status LED animation must update at a stable 50 ms cadence.' },
    @{ Content = $service; Pattern = 'mid_snapshot_read_acquire'; Message = 'Status LED decisions must use the protected snapshot.' },
    @{ Content = $service; Pattern = 'STATUS_LED_BOOT_ANIMATION'; Message = 'Boot animation must have a coordinator-owned indication.' },
    @{ Content = $serviceHeader; Pattern = 'status_led_service_set_boot_animation_active'; Message = 'The startup coordinator must own the full boot LED overlay.' },
    @{ Content = $service; Pattern = 'STATUS_LED_PROVISIONING'; Message = 'BluFi provisioning must have a dedicated cyan indication.' },
    @{ Content = $service; Pattern = 'STATUS_LED_WIFI_CONNECTING'; Message = 'Wi-Fi connection must have a dedicated blue indication.' },
    @{ Content = $service; Pattern = 'STATUS_LED_ACTIVATION'; Message = 'Activation pending must have a dedicated yellow indication.' },
    @{ Content = $service; Pattern = 'STATUS_LED_VOICE'; Message = 'Voice sessions must have a dedicated purple indication.' },
    @{ Content = $service; Pattern = 'STATUS_LED_WARNING'; Message = 'Recoverable network/cloud errors must have an orange indication.' },
    @{ Content = $service; Pattern = 'STATUS_LED_FAULT'; Message = 'Critical faults must take red priority.' },
    @{ Content = $service; Pattern = 'STATUS_LED_CHARGING'; Message = 'Charging indication must be ready for the BMS integration.' },
    @{ Content = $service; Pattern = 'STATUS_LED_SUCCESS_BLINKS 2U'; Message = 'Internet success must flash the whole strip green twice.' },
    @{ Content = $service; Pattern = 'render_success_strip'; Message = 'Internet success must render a whole-strip green flash pattern.' },
    @{ Content = $service; Pattern = 'ws2812_driver_set_all'; Message = 'Whole-strip states must update every LED.' },
    @{ Content = $snapshot; Pattern = 'provisioning_active'; Message = 'The snapshot must expose whether the BluFi window is active.' },
    @{ Content = $wifi; Pattern = 'provisioning_active = ble_provision_is_window_active'; Message = 'Wi-Fi service must publish the BluFi window state.' },
    @{ Content = $manager; Pattern = 'status_led_service_init\(\)'; Message = 'Status LED service must start in the current network/UI test profile.' },
    @{ Content = $cmake; Pattern = 'MID/system/status_led_service\.c'; Message = 'Status LED service must be compiled.' },
    @{ Content = $cmake; Pattern = '(?m)^\s*esp_driver_rmt\s*$'; Message = 'The RMT driver component must be linked.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

if ($driver -match 'gpio_set_level\(LUMIDOCK_WS2812_1_GPIO') {
    throw 'A raw GPIO level cannot encode WS2812 color frames.'
}

if ($driver -match 'return ws2812_driver_show\(\);') {
    throw 'WS2812 driver initialization must not transmit a frame before the status service task starts.'
}

if ($driver -match 'byte_index = symbols_written;') {
    throw 'The RMT callback must convert the symbol offset to a WS2812 data-byte offset.'
}

$faultPriority = $service.IndexOf('if (s_critical_fault) return STATUS_LED_FAULT;')
$bootPriority = $service.IndexOf('if (s_boot_animation_active) return STATUS_LED_BOOT_ANIMATION;')
$successPriority = $service.IndexOf('if (now_us < s_success_until_us) return STATUS_LED_SUCCESS;')
if ($faultPriority -lt 0 -or $bootPriority -lt 0 -or $successPriority -lt 0 -or
    $faultPriority -gt $bootPriority -or $bootPriority -gt $successPriority) {
    throw 'Boot animation must hold the green trailing LED overlay for its full duration; only a critical fault may take priority.'
}

Write-Output 'WS2812 status contract passed.'
