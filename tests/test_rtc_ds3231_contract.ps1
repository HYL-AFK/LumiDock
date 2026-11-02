$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$cmake = Get-Content -Raw -LiteralPath (Join-Path $root 'main\CMakeLists.txt')
$pins = Get-Content -Raw -LiteralPath (Join-Path $root 'main\Dri\board_pins.h')
$ds3231 = Get-Content -Raw -LiteralPath (Join-Path $root 'main\Dri\ds3231.c')
$appRtc = Get-Content -Raw -LiteralPath (Join-Path $root 'main\APP\app_rtc.c')

# 模块目前还没接，驱动靠 LUMIDOCK_DS3231_ENABLE 关在编译门外。开关必须存在
# 且默认是 0，否则接上之前就会跑去唤醒一条空总线。
if ($pins -notmatch 'LUMIDOCK_DS3231_ENABLE\s+0\b') {
    throw 'LUMIDOCK_DS3231_ENABLE must exist and default to 0 while the module is absent.'
}

foreach ($file in @('main\Dri\ds3231.c', 'main\APP\app_rtc.c')) {
    $text = Get-Content -Raw -LiteralPath (Join-Path $root $file)
    if ($text -notmatch '#if !LUMIDOCK_DS3231_ENABLE' -or
        $text -notmatch '#endif /\* LUMIDOCK_DS3231_ENABLE \*/') {
        throw "$file must gate its implementation behind LUMIDOCK_DS3231_ENABLE with a matching #endif."
    }
}

# DS3231 挂在一条独立的 I2C 总线上。ES8311 那条是 I2C_NUM_0（GPIO17/18），
# 两个器件共用一条总线在电气上没问题，但那两个脚已经焊死在音频侧了，新器件
# 只能飞线。这里锁死 port 号，防止以后有人图省事把它挪回去。
if ($pins -notmatch 'LUMIDOCK_DS3231_I2C_PORT\s+I2C_NUM_1') {
    throw 'DS3231 must own a dedicated I2C port (I2C_NUM_1), not share the ES8311 bus.'
}

if ($pins -match 'LUMIDOCK_DS3231_I2C_(SDA|SCL)_GPIO\s+GPIO_NUM_(8|9|17|18)\b') {
    throw 'DS3231 pins must not collide with the ES8311 I2C or I2S pins.'
}

@(
    @{ Content = $cmake; Pattern = 'Dri/ds3231\.c'; Message = 'main/CMakeLists.txt must compile Dri/ds3231.c.' },
    @{ Content = $cmake; Pattern = 'APP/app_rtc\.c'; Message = 'main/CMakeLists.txt must compile APP/app_rtc.c.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

# 新版 i2c_master API。legacy 的 i2c_param_config/i2c_driver_install 和它不能
# 在同一个 port 上混用，而且 I2C_NUM_1 只在新的 master 驱动里好用。
if ($ds3231 -match 'i2c_param_config|i2c_driver_install|i2c_master_write_read_device') {
    throw 'ds3231.c must use the new i2c_master API, not the legacy driver.'
}

if ($ds3231 -notmatch 'i2c_new_master_bus') {
    throw 'ds3231.c must create its own master bus via i2c_new_master_bus().'
}

# OSF 置位时寄存器里是停振残留值（出厂态 2000-01-01），拿去 settimeofday 会让
# 系统时间倒退十几年，TLS 证书全部判"尚未生效"，定位和天气两个 https 请求
# 直接瘫掉——而它们又是 SNTP 之外唯一能拿到正确时间的途径。
if ($ds3231 -notmatch '0x0F|DS3231_REG_STATUS') {
    throw 'ds3231.c must read the status register for the OSF validity check.'
}

if ($ds3231 -notmatch 'OSF') {
    throw 'ds3231.c must reject time values whose oscillator stop flag is set.'
}

# 内部 RAM 一直紧张：websocket 任务连 4096 字节连续内部内存都申请不到过。
# WiTime 那边用 xTaskCreate(rtc_task, 3072) 读芯片，这里改成挂在 time_service_poll
# 的 1 Hz 拍子上，一个字节都不用多花。
if ($appRtc -match 'xTaskCreate|vTaskDelete') {
    throw 'app_rtc must not spawn a task; it hooks into time_service_poll().'
}

# LumiDock 的时区跟着定位结果动态变（app_time_apply_timezone_offset 会改 TZ），
# 芯片里必须存 UTC。存本地时间的话换个时区读回来就和系统对不上。
if ($appRtc -match 'mktime|localtime\(') {
    throw 'app_rtc must keep the chip in UTC (use gmtime_r), not local time.'
}

if ($appRtc -notmatch 'gmtime_r') {
    throw 'app_rtc must convert the system clock to UTC with gmtime_r before writing back.'
}

# 温度走 NTC，天气温度走网络，两条线都不该被 RTC 顶掉。这段是上一版
# no_external_rtc_test.ps1 留下来的约束，语义没变，仍然要守。
$sources = Get-ChildItem -Path (Join-Path $root 'main') -Recurse -File -Include *.c,*.h |
    Get-Content -Raw

if ($sources -match '(?i)WEATHER_TEMP_SOURCE_RTC|set_rtc_temperature|LUMIDOCK_DISABLE_EXTERNAL_RTC|RTC 温度') {
    throw 'The RTC temperature must not be used as the weather or board temperature source.'
}

Write-Output 'PASS: DS3231 RTC contract holds (dedicated I2C port, OSF check, UTC storage, no task)'
