$ErrorActionPreference = 'Stop'

$source = Get-Content -Raw 'main/APP/app_time.c'
$sdkconfig = Get-Content -Raw 'sdkconfig'

if ($sdkconfig -notmatch '(?m)^CONFIG_LWIP_SNTP_MAX_SERVERS=2\s*$') {
    throw 'sdkconfig must allow two SNTP servers'
}

foreach ($required in @(
    'esp_netif_sntp_deinit\(\)',
    'SNTP_MAX_ATTEMPTS',
    's_sntp_attempt_count',
    's_sntp_retry_at_us',
    'esp_netif_sntp_init\(&cfg\)'
)) {
    if ($source -notmatch $required) {
        throw "SNTP retry implementation is missing: $required"
    }
}

if ($source -notmatch '(?s)esp_netif_sntp_deinit\(\).*?s_sntp_initialized\s*=\s*false') {
    throw 'SNTP deinit must clear the initialized state'
}

if ($source -match 'app_rtc_set_from_system') {
    throw 'SNTP success path must not depend on an external RTC'
}

Write-Output 'sntp_retry_policy_test: PASS'

