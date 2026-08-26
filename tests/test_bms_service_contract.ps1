$ErrorActionPreference = 'Stop'
$servicePath = "$PSScriptRoot\..\main\MID\protocol\bms_service.c"
$protocolPath = "$PSScriptRoot\..\main\MID\protocol\bms_protocol.c"
$pinsPath = "$PSScriptRoot\..\main\Dri\board_pins.h"
$managerPath = "$PSScriptRoot\..\main\APP\app_manager.c"

$service = Get-Content -Raw -LiteralPath $servicePath
$protocol = Get-Content -Raw -LiteralPath $protocolPath
$pins = Get-Content -Raw -LiteralPath $pinsPath
$manager = Get-Content -Raw -LiteralPath $managerPath

if ($pins -notmatch 'LUMIDOCK_BMS_UART_PORT\s+UART_NUM_2' -or
    $pins -notmatch 'LUMIDOCK_BMS_TX_GPIO\s+GPIO_NUM_39' -or
    $pins -notmatch 'LUMIDOCK_BMS_RX_GPIO\s+GPIO_NUM_40') {
    throw 'BMS must retain the LumiDock UART2 GPIO39/40 board contract.'
}
if ($service -notmatch 'header_prefix_keep_len' -or $service -notmatch 'append_rx_bytes') {
    throw 'BMS RX must preserve partial headers and recover from bounded buffer pressure.'
}
if ($service -notmatch 'try_crc_locked_candidate' -or
    $service -notmatch 'if \(s_frame_len < BMS_PROTOCOL_FRAME_LEN_14S\)' -or
    $service -notmatch 'if \(s_frame_len < BMS_PROTOCOL_FRAME_LEN_132\)') {
    throw 'BMS RX must CRC-lock 128, then wait for 130, then wait for 132-byte candidates.'
}
if ($service -notmatch 'CRC 已确认帧边界' -or
    $service -notmatch 'consume_bytes\(frame_len\)') {
    throw 'A CRC-locked but semantically invalid BMS frame must consume its full boundary.'
}
if ($service -notmatch 'discard_stale_response_before_request' -or
    $service -notmatch 'BMS previous response remainder' -or
    $service -notmatch 'discard_stale_response_before_request\(\)') {
    throw 'BMS must clear a prior partial response before starting the next request cycle.'
}
if ($service -notmatch 'bms_values_are_safe' -or
    $service -notmatch 'last_rx_ms\s*=' -or
    $service -notmatch 'last_parse_ms\s*=') {
    throw 'BMS accepted frames must pass safety validation and record receive timestamps.'
}
if ($service -notmatch 's_frame_len\s*=\s*0;' -or
    $service -notmatch 'BMS receive timeout') {
    throw 'BMS timeout must clear stale partial data before the next request.'
}
if ($protocol -notmatch 'parsed\.cell_mv\[i\] < 1500' -or
    $protocol -notmatch 'parsed\.temp_c\[i\] < -40' -or
    $protocol -notmatch 'parsed\.rsoc_percent > 100') {
    throw 'BMS protocol must reject implausible cell, temperature, and state-of-charge fields.'
}
if ($manager -notmatch '#if !LUMIDOCK_NETWORK_UI_TEST_MODE\s*\r?\n\s*if \(\(err = button_driver_init\(\)' -or
    $manager -notmatch 'if \(\(err = bms_service_init\(\)\)') {
    throw 'BMS must remain disabled while the bench network/UI test profile is enabled.'
}

Write-Output 'BMS service contract checks passed.'
