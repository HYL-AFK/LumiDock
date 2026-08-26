$ErrorActionPreference = 'Stop'

$source = Get-Content -Raw 'main/Dri/wifi_manager.c'

if ($source -notmatch 'IP_EVENT_STA_LOST_IP') {
    throw 'wifi manager does not handle IP_EVENT_STA_LOST_IP'
}

$lostIpBranch = [regex]::Match(
    $source,
    '(?s)else if \(event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP\).*?(?=\n    else if|\n\})'
).Value

if ([string]::IsNullOrWhiteSpace($lostIpBranch)) {
    throw 'IP_EVENT_STA_LOST_IP branch is empty or missing'
}

foreach ($required in @(
    'clear_sta_runtime_state\(\)',
    'notify_status_changed\(\)',
    'wifi_manager_reconnect_sta\(\)'
)) {
    if ($lostIpBranch -notmatch $required) {
        throw "lost-IP branch is missing: $required"
    }
}

if ($source -notmatch '(?s)static void clear_sta_runtime_state\(void\).*?s_sta_connected = false.*?reset_internet_health\(\).*?strlcpy\(s_sta_ip_str, "0\.0\.0\.0"') {
    throw 'STA runtime clear helper does not reset connection, internet, and IP state'
}

Write-Output 'wifi_lost_ip_handler_test: PASS'

