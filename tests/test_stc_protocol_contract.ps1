$ErrorActionPreference = 'Stop'
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"
$pins = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\board_pins.h"
$protocolPath = "$PSScriptRoot\..\main\MID\protocol\stc_protocol.h"
$servicePath = "$PSScriptRoot\..\main\MID\protocol\stc_service.c"
if (-not (Test-Path -LiteralPath $protocolPath) -or -not (Test-Path -LiteralPath $servicePath)) {
    throw 'STC protocol and service modules are missing.'
}
$protocol = Get-Content -Raw -LiteralPath $protocolPath
$protocolImpl = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\protocol\stc_protocol.c"
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
    $protocol -notmatch 'STC_PROTOCOL_FRAME_LEN_RESPONSE\s+41' -or
    $protocol -notmatch 'STC_PROTOCOL_RESPONSE_LENGTH_BYTE\s+0x29' -or
    $protocol -notmatch 'STC_PROTOCOL_HEADER_BYTE_0\s+0x53' -or
    $protocol -notmatch 'STC_PROTOCOL_TAIL_BYTE_1\s+0x21') {
    throw 'STC protocol constants must match the V2.2 12/41-byte document.'
}

# V2.2 把每路遥测从 9 字节扩到 10 字节（第 10 字节是快充协议码），CRC 覆盖
# [0..36] 并落在偏移 37。这里用文件里的真实常量做一次字节算术回验，避免
# base / stride / count / crc offset 各改各的、彼此不自洽。
if ($protocol -notmatch 'STC_PROTOCOL_CHANNEL_BASE\s+(\d+)U?') { throw 'Missing STC_PROTOCOL_CHANNEL_BASE.' }
$chanBase = [int]$Matches[1]
if ($protocol -notmatch 'STC_PROTOCOL_CHANNEL_STRIDE\s+(\d+)U?') { throw 'Missing STC_PROTOCOL_CHANNEL_STRIDE.' }
$chanStride = [int]$Matches[1]
if ($protocol -notmatch 'STC_PROTOCOL_CHANNEL_COUNT\s+(\d+)U?') { throw 'Missing STC_PROTOCOL_CHANNEL_COUNT.' }
$chanCount = [int]$Matches[1]
if ($protocol -notmatch 'STC_PROTOCOL_RESPONSE_CRC_OFFSET\s+(\d+)U?') { throw 'Missing STC_PROTOCOL_RESPONSE_CRC_OFFSET.' }
$crcOffset = [int]$Matches[1]

if ($chanStride -ne 10) {
    throw 'V2.2 raised the per-channel stride to 10 bytes because the protocol code is the 10th.'
}
if (($chanBase + $chanStride * $chanCount) -ne $crcOffset) {
    throw "V2.2 layout is inconsistent: base($chanBase) + stride($chanStride) * count($chanCount) must equal the CRC offset ($crcOffset)."
}
if ($protocolImpl -notmatch 'ch->protocol_code = frame\[base \+ 9U\]') {
    throw 'The protocol code must be parsed from the 10th byte of every channel.'
}
if ($service -notmatch 'dst->protocol_code = response\.channels\[idx\]\.protocol_code') {
    throw 'The parsed protocol code must be published in the STC snapshot.'
}
if ($protocol -notmatch 'STC_PROTOCOL_TYPE_QC2_0\s+0x01U' -or
    $protocol -notmatch 'STC_PROTOCOL_TYPE_PD_PPS\s+0x06U' -or
    $protocol -notmatch 'STC_PROTOCOL_TYPE_AFC\s+0x0BU') {
    throw 'The protocol-code type table must match the V2.2 document.'
}

# 协议码最终会显示在顶栏和充电页上，而这两处只挂了 Montserrat 14/20。它们的
# cmap 第一段是 32 起、95 长，即只覆盖 U+0020~U+007E（外加 62 个稀疏符号）。
# 所以把这条链路固定成断言：字体必须仍然覆盖可打印 ASCII，且发布给 UI 的
# 协议名必须是纯 ASCII，否则屏幕上只会出现空白方块。
foreach ($fontName in @('lv_font_montserrat_14', 'lv_font_montserrat_20')) {
    $font = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\managed_components\lvgl__lvgl\src\font\$fontName.c"
    if ($font -notmatch 'range_start = 32, .range_length = 95') {
        throw "$fontName no longer covers printable ASCII; the protocol labels would render as blanks."
    }
}
foreach ($label in @('QC2.0', 'QC3.0', 'FCP', 'SCP', 'PD2.0', 'PD3.0', 'PD PPS',
                     'PE1.1', 'PE2.0', 'VOOC', 'SFCP', 'AFC', 'NONE')) {
    if ($service -notmatch ('name = "' + [regex]::Escape($label) + '"')) {
        throw "The STC service name table is missing the protocol label '$label'."
    }
}
foreach ($entry in [regex]::Matches($service, 'name = "([^"]*)"')) {
    if ($entry.Groups[1].Value -notmatch '^[\x20-\x7E]+$') {
        throw 'Protocol labels must stay printable ASCII for the Montserrat UI fonts.'
    }
}
if ($service -notmatch 'static void publish_charging\(void\)' -or
    $service -notmatch 'publish_charging\(\);' -or
    $service -notmatch 'mid_snapshot_set_charging\(&charging\)') {
    throw 'The negotiated protocol must reach the UI through the charging snapshot.'
}

$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"
if ($ui -notmatch 'snapshot->charging\.name') {
    throw 'The status bar and charging page must render the published protocol name.'
}
if ($ui -notmatch 'lv_obj_align_to\(bar->clock, bar->protocol') {
    throw 'The status-bar clock must follow the protocol label width, or a label like "PD PPS" overlaps it.'
}
# 无 STC 数据时不能回退成写死的 "PD"：那样“协议未知”和“真的协商到 PD”在屏幕上
# 长得一模一样，正好违背了这一整条“显示真实协议”的需求。
if ($ui -match 'charging\.name : "PD"' -or $ui -match 'make_label\([^)]*"PD",') {
    throw 'A missing STC snapshot must not be rendered as a hardcoded protocol label.'
}
if ($ui -notmatch 'charging\.name : "--"') {
    throw 'The protocol label must fall back to an explicit placeholder when STC is offline.'
}

# 电压档位判断归 STC：ESP32 只能上报自身状态码，不得再下发档位指令。
if ($protocol -match 'STC_PROTOCOL_CMD_') {
    throw 'The ESP32 protocol header must not carry gear command codes.'
}
if ($protocol -notmatch 'STC_PROTOCOL_ESP32_STATUS_OK\s+0x00') {
    throw 'Offset 7 must be documented as the ESP32 status code.'
}
if ($service -notmatch 'STC_PROTOCOL_ESP32_STATUS_OK') {
    throw 'The request frame must carry the ESP32 status code, not a gear command.'
}
if ($service -match 'STC_THERMAL_|STC_GEAR_CMD_FOR_MV|STC_TEMP_LIMIT_GEAR_MV|s_thermal_') {
    throw 'The ESP32 must not keep its own thermal derating state machine.'
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

