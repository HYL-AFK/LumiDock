$ErrorActionPreference = 'Stop'
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
@(
    @{ Pattern = 'ping_interval_sec\s*=\s*10'; Message = 'Ping interval must be 10 seconds.' },
    @{ Pattern = 'pingpong_timeout_sec\s*=\s*20'; Message = 'Pong timeout must be 20 seconds.' },
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
Write-Output 'WebSocket health contract passed.'
