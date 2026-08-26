$ErrorActionPreference = 'Stop'
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"
$pins = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\board_pins.h"
$protocolPath = "$PSScriptRoot\..\main\MID\protocol\stc_protocol.h"
$servicePath = "$PSScriptRoot\..\main\MID\protocol\stc_service.c"
if (-not (Test-Path -LiteralPath $protocolPath) -or -not (Test-Path -LiteralPath $servicePath)) {
    throw 'STC protocol and service modules are missing.'
}
$protocol = Get-Content -Raw -LiteralPath $protocolPath
$service = Get-Content -Raw -LiteralPath $servicePath
$snapshot = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\mid_snapshot.h"
$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"
$sdkconfig = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\sdkconfig"

if ($cmake -notmatch 'MID/protocol/stc_protocol\.c' -or $cmake -notmatch 'MID/protocol/stc_service\.c') {
    throw 'STC protocol and service sources must be part of the main component.'
}
if ($pins -notmatch 'LUMIDOCK_STC_UART_PORT\s+UART_NUM_0' -or
    $pins -notmatch 'LUMIDOCK_STC_TX_GPIO\s+GPIO_NUM_43' -or
    $pins -notmatch 'LUMIDOCK_STC_RX_GPIO\s+GPIO_NUM_44' -or
    $pins -notmatch 'LUMIDOCK_BMS_TX_GPIO\s+GPIO_NUM_39' -or
    $pins -notmatch 'LUMIDOCK_BMS_RX_GPIO\s+GPIO_NUM_40') {
    throw 'STC must use top UART0 TX/RX while BMS remains on GPIO39/40.'
}
if ($protocol -notmatch 'STC_PROTOCOL_FRAME_LEN_REQUEST\s+12' -or
    $protocol -notmatch 'STC_PROTOCOL_FRAME_LEN_RESPONSE\s+21' -or
    $protocol -notmatch 'STC_PROTOCOL_HEADER_BYTE_0\s+0x53' -or
    $protocol -notmatch 'STC_PROTOCOL_TAIL_BYTE_1\s+0x21') {
    throw 'STC protocol constants must match the 12/21-byte document.'
}
if ($service -notmatch 'UART_BAUD_RATE\s+9600' -or
    $service -notmatch 'uart_param_config' -or
    $service -notmatch 'stc_protocol_build_request' -or
    $service -notmatch 'stc_protocol_parse_response') {
    throw 'STC service must configure 9600 8N1 and use the protocol codec.'
}
if ($snapshot -notmatch 'stc_snapshot_t' -or $snapshot -notmatch 'stc_snapshot_t\s+stc') {
    throw 'The middleware snapshot must expose STC status separately from BMS data.'
}
if ($manager -notmatch 'stc_service_init') {
    throw 'The application manager must initialize the STC service.'
}
if ($sdkconfig -notmatch 'CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y' -or
    $sdkconfig -notmatch 'CONFIG_ESP_CONSOLE_UART_NUM=-1' -or
    ($sdkconfig -notmatch '# CONFIG_ESP_CONSOLE_UART is not set' -and
     $sdkconfig -notmatch '# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set')) {
    throw 'UART0 console must move to USB Serial JTAG before STC owns UART0.'
}

Write-Output 'STC protocol contract checks passed.'

