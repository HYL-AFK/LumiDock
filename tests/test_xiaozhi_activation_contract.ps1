$ErrorActionPreference = 'Stop'

$service = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_activation_service.c"
$header = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_activation_service.h"
$snapshotHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\mid_snapshot.h"
$snapshotSource = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\mid_snapshot.c"
$wifi = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\network\wifi_service.c"
$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"
$manifest = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\idf_component.yml"

@(
    @{ Content = $service; Pattern = 'https://api\.tenclass\.net/xiaozhi/ota/'; Message = 'Activation must use the official xiaozhi.me OTA endpoint.' },
    @{ Content = $service; Pattern = 'backend: official xiaozhi\.me'; Message = 'COM6 must identify the official xiaozhi.me backend.' },
    @{ Content = $service; Pattern = 'copy_json_string\(websocket, "url"'; Message = 'OTA must keep requiring a websocket URL.' },
    @{ Content = $service; Pattern = 'copy_json_string\(websocket, "token"'; Message = 'OTA must keep requiring a websocket token.' },
    @{ Content = $service; Pattern = 'activation'; Message = 'The service must parse the server activation object.' },
    @{ Content = $service; Pattern = '"code"'; Message = 'The service must read the server-issued activation code.' },
    @{ Content = $service; Pattern = 'nvs_get_str'; Message = 'The client UUID must be loaded from NVS.' },
    @{ Content = $service; Pattern = 'nvs_set_str'; Message = 'A newly generated client UUID must be saved to NVS.' },
    @{ Content = $service; Pattern = 'esp_wifi_get_mac'; Message = 'The registration request must identify the physical device with its STA MAC.' },
    @{ Content = $service; Pattern = 'voice_activation_snapshot_t'; Message = 'Activation state must be published for the UI rather than updating UI from the HTTP callback.' },
    @{ Content = $header; Pattern = 'xiaozhi_activation_service_notify_network'; Message = 'Wi-Fi service must be able to trigger activation after internet verification.' },
    @{ Content = $snapshotHeader; Pattern = 'voice_activation_snapshot_t'; Message = 'The snapshot contract must carry activation UI data.' },
    @{ Content = $snapshotSource; Pattern = 'mid_snapshot_set_voice_activation'; Message = 'The snapshot module must publish activation UI data.' },
    @{ Content = $wifi; Pattern = 'xiaozhi_activation_service_notify_network'; Message = 'Activation must begin only after the existing internet probe succeeds.' },
    @{ Content = $ui; Pattern = 'UI_PAGE_ACTIVATION'; Message = 'An unbound device must show the real server activation code.' },
    @{ Content = $cmake; Pattern = 'MID/voice/xiaozhi_activation_service.c'; Message = 'The activation service must be compiled.' },
    @{ Content = $cmake; Pattern = '(?m)^\s*esp_app_format\s*$'; Message = 'The activation service needs the ESP-IDF esp_app_format component for esp_app_desc.h.' },
    @{ Content = $manifest; Pattern = 'espressif/esp_websocket_client'; Message = 'The XiaoZhi WSS dependency must be declared for the next voice stage.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

if ($service -match 'qrcode') {
    throw 'The supplied XiaoZhi reference returns an activation code, not a fabricated QR URL.'
}

Write-Output 'XiaoZhi activation contract passed.'

