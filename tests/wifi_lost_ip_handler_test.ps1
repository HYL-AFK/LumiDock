$ErrorActionPreference = 'Stop'

$source = Get-Content -Raw 'main/Dri/wifi_manager.c'

if ($source -notmatch 'IP_EVENT_STA_LOST_IP') {
    throw 'wifi manager does not handle IP_EVENT_STA_LOST_IP'
}

# LOST_IP 的处理在重构后分成两段：IP 事件分发器只把 IP_EVENT_STA_LOST_IP 映射成
# NETWORK_EVENT_STA_LOST_IP，真正的清态 / 通知 / 重连落在 network_coordinator_task 的
# NETWORK_EVENT_STA_LOST_IP 分支里。
# 断言必须跟着行为走。原来它绑死在事件分发器那一侧，重构之后就一直红着，而它想守的
# 三件事其实一直都在——绑错位置的契约比没有契约更糟，它会训练人忽略红线。
if ($source -notmatch 'IP_EVENT_STA_LOST_IP\)\s*event\.type = NETWORK_EVENT_STA_LOST_IP') {
    throw 'The IP event dispatcher must map IP_EVENT_STA_LOST_IP to NETWORK_EVENT_STA_LOST_IP; without it the lost-IP handler is unreachable.'
}

$lostIpBranch = [regex]::Match(
    $source,
    '(?s)else if \(event_id == NETWORK_EVENT_STA_LOST_IP\).*?(?=\n    else if|\n\})'
).Value

if ([string]::IsNullOrWhiteSpace($lostIpBranch)) {
    throw 'NETWORK_EVENT_STA_LOST_IP branch is empty or missing'
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

