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

# 探测成功不等于可以永久停机。IP 层假死时 WiFi 关联和 IP 都不变，
# WIFI_EVENT_STA_DISCONNECTED / NETWORK_EVENT_STA_GOT_IP 一个都不会来；探测任务若
# 睡到“连接状态变化”就再也不会醒，连续失败重连的规则永远没机会执行。实测表现是
# 电台 11 次 http_open 全报 28674、WSS 的 getaddrinfo 全报 202，而界面仍打印
# internet=1，设备一直卡到重启。
if ($wifi -notmatch 'INTERNET_PROBE_ONLINE_POLL_INTERVAL_MS') {
    throw 'Internet probe must keep polling while online; otherwise an IP-layer silent failure can never trigger the reconnect rule.'
}
if ($wifi -match '(?s)if \(get_internet_online\(\)\)\s*\{\s*internet_probe_wait_for_connection_change\(\);') {
    throw 'The online branch must not park the probe task until a connection-state event; that deadlock is what blocks self-healing.'
}
if ($wifi -notmatch '(?s)get_internet_online\(\)\s*\?\s*INTERNET_PROBE_ONLINE_POLL_INTERVAL_MS') {
    throw 'The probe loop must select the online poll interval while the link is considered up.'
}
# 在线巡检必须是“定时自醒”而不是纯 vTaskDelay：主动重连后的 GOT_IP 会带一个
# 任务通知，延时期间必须能被它提前打断，否则重连后要白等一个巡检周期才复测。
if ($wifi -notmatch '(?s)internet_probe_delay\(uint32_t delay_ms\)[\s\S]{0,900}?ulTaskNotifyTake\(pdTRUE, current\)') {
    throw 'The probe delay must be interruptible by the connection-change notification so a fresh reconnect is retested immediately.'
}

# 判定太慢本身就是故障的一部分。实测那次链路假死拖了 33 秒：电台先饿死（HLS 分片读超时、
# 重拉列表失败、最后 queued=0），用户唤醒时只能冷启动去撞最坏的网络，DNS 直接 EAI_FAIL，
# 全程只响一声错误音。而旧规则是"连续失败 3 次"，每次探测顶格耗 5 秒超时、失败后还要再等
# 5 秒——最坏 35 秒以上，判定比它要修的那次故障还长。
#
# 修法不是简单调小数字，而是先承认"压根连不上"和"有回包但不合规格"不是一回事：前者说明
# 链路已经不能把一条新的出网连接送出去（正是假死的形态），必须尽快收场；后者说明链路是通的，
# 重连帮不上忙，多观察几轮反而更稳。
$policyHeader = Get-Content -Raw 'main/Dri/internet_health_policy.h'
$policy = Get-Content -Raw 'main/Dri/internet_health_policy.c'

if ($policyHeader -notmatch 'INTERNET_PROBE_OUTCOME_HARD_FAILURE') {
    throw 'The probe must tell "no new outbound connection could be opened" apart from "the endpoint answered with an unusable response"; one bare bool cannot carry that.'
}
if ($wifi -notmatch '(?s)err != ESP_OK[\s\S]{0,200}?INTERNET_PROBE_OUTCOME_HARD_FAILURE') {
    throw 'A request that never got a response is exactly what unreachable means; the classification must be driven by the transport error, not by the HTTP status.'
}
if ($policy -notmatch 'INTERNET_HEALTH_HARD_FAILURE_LIMIT\s+2U') {
    throw 'An unreachable link must be declared after two strikes, not three: the old threshold outlives the outage it is meant to fix.'
}
if ($policy -notmatch '(?s)outcome == INTERNET_PROBE_OUTCOME_HARD_FAILURE[\s\S]{0,200}?consecutive_hard_failures\+\+') {
    throw 'Hard failures must be counted separately from soft ones, or the faster threshold can never be reached.'
}
if ($policy -notmatch '(?s)consecutive_failures >= INTERNET_HEALTH_FAILURE_LIMIT\s*\|\|[\s\S]{0,160}?consecutive_hard_failures >= INTERNET_HEALTH_HARD_FAILURE_LIMIT') {
    throw 'Either counter must be able to trigger the reconnect on its own; one is not a tie-breaker for the other.'
}
if ($policy -notmatch '(?s)INTERNET_PROBE_OUTCOME_OK[\s\S]{0,240}?consecutive_hard_failures = 0') {
    throw 'A recovered link must clear the hard-failure streak, otherwise the next blip adds to it and turns ordinary jitter into a real reconnect.'
}
if ($wifi -notmatch 'INTERNET_PROBE_UNHEALTHY_POLL_INTERVAL_MS\s+2000') {
    throw 'While unhealthy the probe must retry on a short cadence; once the timeout is fixed the failure count is the only variable left to compress.'
}
if ($wifi -notmatch '(?s)get_internet_probe_failures\(\) > 0\s*\?\s*INTERNET_PROBE_UNHEALTHY_POLL_INTERVAL_MS') {
    throw 'The unhealthy cadence must take priority over both the online and offline intervals in the probe loop.'
}
if ($wifi -notmatch '(?s)internet probe declared the link dead[\s\S]{0,320}?unreachable') {
    throw 'The reconnect log must say which signal fired it; a hard failure and a soft one are different incidents and must not be summarised into one line.'
}

Write-Output 'internet_probe_gate_test: PASS'

