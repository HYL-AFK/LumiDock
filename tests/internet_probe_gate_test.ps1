$ErrorActionPreference = 'Stop'

$wifi = Get-Content -Raw 'main/Dri/wifi_manager.c'
$service = Get-Content -Raw 'main/MID/network/wifi_service.c'

if ($wifi -notmatch 'INTERNET_PROBE_URL\s+"https?://www\.qq\.com/favicon\.ico"') {
    throw 'internet probe must request the confirmed Tencent favicon resource'
}

if ($service -notmatch 'wifi_manager_is_internet_online\(\)') {
    throw 'wifi service must gate network services on internet probe state'
}

if ($service -notmatch '(?s)sta_online\s*&&\s*internet_online\s*&&\s*s_network_services_ready') {
    throw 'weather/cloud notifications must wait for internet probe success'
}

if ($wifi -notmatch 'HTTP_EVENT_ON_DATA') {
    throw 'internet probe must observe response body data'
}

if ($wifi -notmatch 'content_type') {
    throw 'internet probe must track response content type'
}

if ($wifi -notmatch '(?s)body_bytes\s*>\s*0') {
    throw 'internet probe must require a non-empty response body'
}

if ($wifi -notmatch '\.disable_auto_redirect\s*=\s*true') {
    throw 'the Tencent probe must not follow a redirect to an unrelated CDN'
}

Write-Output 'internet_probe_gate_test: PASS'

