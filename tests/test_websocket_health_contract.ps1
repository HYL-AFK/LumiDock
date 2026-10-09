$ErrorActionPreference = 'Stop'
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
# 心跳参数必须走宏：日志曾经把值写死成 20，而配置早就是 40，于是每次连上都打印
# pong_timeout_sec=20 —— 排查的人会以为参数没生效、朝错误方向找（实测日志 1767 行）。
# 所以这里既锁宏的值，也锁"配置取值必须来自宏"，两边不许再分家。
@(
    @{ Pattern = '#define XIAOZHI_WSS_PING_INTERVAL_SEC 10'; Message = 'Ping interval must stay 10 seconds.' },
    @{ Pattern = '#define XIAOZHI_WSS_PONGPONG_TIMEOUT_SEC 40'; Message = 'Pong timeout must stay 40 seconds; 20 misjudged slow public links as dead.' },
    @{ Pattern = 'ping_interval_sec\s*=\s*XIAOZHI_WSS_PING_INTERVAL_SEC'; Message = 'The client config must take the ping interval from the shared macro.' },
    @{ Pattern = 'pingpong_timeout_sec\s*=\s*XIAOZHI_WSS_PONGPONG_TIMEOUT_SEC'; Message = 'The client config must take the pong timeout from the shared macro.' },
    @{ Pattern = 'XIAOZHI_WSS_HEALTH_(HEALTHY|STALE|FAILED|CLOSED)'; Message = 'WSS health states are missing.' },
    @{ Pattern = 's_wss_last_ping_us'; Message = 'Last Ping timestamp is missing.' },
    @{ Pattern = 's_wss_last_pong_us'; Message = 'Last Pong timestamp is missing.' },
    @{ Pattern = 'WSS pong received'; Message = 'Pong logging is missing.' },
    @{ Pattern = 'WSS transport diagnostics'; Message = 'TLS/socket/close diagnostics are missing.' },
    @{ Pattern = 'esp_tls_last_esp_err'; Message = 'TLS error diagnostics are missing.' },
    @{ Pattern = 'esp_transport_sock_errno'; Message = 'Socket errno diagnostics are missing.' },
    @{ Pattern = 'close_status_code'; Message = 'WebSocket close code diagnostics are missing.' }
) | ForEach-Object { if ($voice -notmatch $_.Pattern) { throw $_.Message } }
if ($voice -notmatch 'connected_before' -or $voice -notmatch 'connected_after' -or
    $voice -notmatch 's_wss_health != XIAOZHI_WSS_HEALTH_STALE') {
    throw 'Binary sends must log and gate on connection health.'
}
# 反向断言：日志不许再把超时值写死，否则打印值和生效值又会分家。
if ($voice -match 'pong_timeout_sec=20|no PONG within 20 seconds') {
    throw 'Heartbeat logs must print the configured timeout instead of a hard-coded value.'
}
Write-Output 'WebSocket health contract passed.'
