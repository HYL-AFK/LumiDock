$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$header = Get-Content -Raw -LiteralPath (Join-Path $root 'main\COM\fault_registry.h')
$source = Get-Content -Raw -LiteralPath (Join-Path $root 'main\COM\fault_registry.c')
$cmake = Get-Content -Raw -LiteralPath (Join-Path $root 'main\CMakeLists.txt')
$statusLed = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\system\status_led_service.c')
$timeService = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\network\time_service.c')
$watchdog = Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\system\watchdog_service.c')

# --- 设施本身 ---
if ($cmake -notmatch '"COM/fault_registry\.c"') {
    throw 'fault_registry.c must be registered in main/CMakeLists.txt.'
}

foreach ($level in 1, 2, 3) {
    if ($header -notmatch "FAULT_LEVEL_$level\s*=\s*$level") {
        throw "FAULT_LEVEL_$level must exist with the value $level."
    }
}

# 等级1 的重启窗口是产品语义，改它等于改设备行为，必须显式确认。
if ($header -notmatch 'FAULT_LEVEL1_REBOOT_TIMEOUT_S\s+\(30U\s*\*\s*60U\)') {
    throw 'Level-1 faults must reboot after 30 minutes (FAULT_LEVEL1_REBOOT_TIMEOUT_S).'
}

# --- 编码规则：P + 等级 + 两位序号，码值必须和等级一致 ---
$codes = [regex]::Matches($header, 'FAULT_P(\d)(\d\d)_\w+\s*=\s*0x([0-9a-fA-F]+)')
if ($codes.Count -lt 12) {
    throw "Expected at least 12 fault codes, found $($codes.Count)."
}
foreach ($match in $codes) {
    $declaredLevel = [int]$match.Groups[1].Value
    $value = [Convert]::ToInt32($match.Groups[3].Value, 16)
    if ([math]::Floor($value / 256) -ne $declaredLevel) {
        throw "Fault code $($match.Value) encodes level $declaredLevel but its value is 0x$($match.Groups[3].Value)."
    }
}

# --- 持久化：故障表必须跨重启存活 ---
if ($source -notmatch 'nvs_set_blob') {
    throw 'Fault table must persist to NVS; expected nvs_set_blob in fault_registry.c.'
}
# 落在 NVS 里的 blob 布局变了就得升 version，否则旧记录会被按新结构解释。
foreach ($staticAssert in @('fault entry layout changed', 'fault header layout changed', 'fault NVS blob size changed')) {
    if ($source -notmatch [regex]::Escape($staticAssert)) {
        throw "fault_registry.c must keep the layout _Static_assert: $staticAssert"
    }
}

# --- 开机时必须摘掉等级1 的挂载位 ---
# 等级1 的处置手段就是重启。若重启后它仍算“挂着”，30 分钟后又触发一次，
# 设备会永远停在同一个重启循环里。等级2/3 相反，必须保留。
if ($source -notmatch 'entry->level == \(uint8_t\)FAULT_LEVEL_1') {
    throw 'fault_registry_init() must detach level-1 faults on boot or the device loops on reboot.'
}
if ($source -notmatch 'cleared_level1') {
    throw 'Boot-time level-1 clearing must be counted and logged so the loop is visible in the serial log.'
}

# --- 等级1 的复位出口 ---
# 必须复用看门狗那条闩锁，而不是直接 esp_restart()：直接重启会跳过错峰收尾，
# 现场也留不下来。
if ($source -match 'esp_restart\s*\(') {
    throw 'fault_registry must not call esp_restart() directly; route through watchdog_service_request_reset().'
}
if ($timeService -notmatch 'fault_registry_level1_overdue\(\)') {
    throw 'time_service_poll() must evaluate fault_registry_level1_overdue().'
}
if ($timeService -notmatch 'watchdog_service_request_reset\s*\(') {
    throw 'Level-1 expiry must request a reset through watchdog_service_request_reset().'
}
if ($watchdog -notmatch 'watchdog_service_request_reset') {
    throw 'watchdog_service must implement watchdog_service_request_reset().'
}

# --- 灯效：三个等级都要有反馈 ---
foreach ($pair in @(
    @('FAULT_LEVEL_1', 'STATUS_LED_FAULT'),
    @('FAULT_LEVEL_2', 'STATUS_LED_WARNING'),
    @('FAULT_LEVEL_3', 'STATUS_LED_DIAGNOSTIC'))) {
    if ($statusLed -notmatch "$($pair[0])\)\s*return\s*$($pair[1])") {
        throw "$($pair[0]) must map to $($pair[1]) in status_led_service.c."
    }
}
if ($statusLed -notmatch 'case STATUS_LED_DIAGNOSTIC') {
    throw 'STATUS_LED_DIAGNOSTIC needs a render branch in status_led_service.c.'
}

# --- 不许在这个模块里开任务 ---
# 故障挂载会被大量业务路径调用，如果再起一条任务，栈和优先级又要重新论证一遍。
if ($source -match 'xTaskCreate') {
    throw 'fault_registry must not create tasks; keep it callback-only.'
}

# --- 业务接入面 ---
$raised = [regex]::Matches($source + (Get-Content -Raw -LiteralPath (Join-Path $root 'main\Dri\wifi_manager.c')) +
    (Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\protocol\bms_service.c')) +
    (Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\protocol\stc_service.c')) +
    (Get-Content -Raw -LiteralPath (Join-Path $root 'main\MID\system\thermal_service.c')) +
    (Get-Content -Raw -LiteralPath (Join-Path $root 'main\APP\app_time.c')) +
    (Get-Content -Raw -LiteralPath (Join-Path $root 'main\APP\app_rtc.c')), 'fault_registry_raise\s*\(')
if ($raised.Count -lt 8) {
    throw "Expected at least 8 call sites raising faults, found $($raised.Count)."
}

Write-Output 'PASS: fault registry contract holds (P-codes, 3 levels, NVS persistence, LED mapping, watchdog reset path)'
