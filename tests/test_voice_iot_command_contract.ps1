$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$iot = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\voice_iot_command_service.c"
$timer = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\timer_service.c"
$led = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\status_led_service.c"
$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"
$iot_bytes = [System.IO.File]::ReadAllBytes("$PSScriptRoot\..\main\MID\system\voice_iot_command_service.c")
$iot_hex = [BitConverter]::ToString($iot_bytes)

@(
    @{ Content = $voice; Pattern = 'strcmp\(type->valuestring, "stt"\)'; Message = 'Voice service must inspect cloud STT before accepting a response TTS.' },
    @{ Content = $voice; Pattern = 'voice_iot_command_service_execute'; Message = 'Voice service must delegate local commands to the owned IoT service.' },
    @{ Content = $voice; Pattern = 'local IoT command handled:'; Message = 'COM6 must identify each local command and execution result.' },
    @{ Content = $iot; Pattern = 'status_led_service_set_user_enabled'; Message = 'Light-strip control must stay owned by the status LED service.' },
    @{ Content = $iot; Pattern = 'ui_service_request_exit_standby'; Message = 'Standby exit must be posted to the UI-owning task.' },
    @{ Content = $led; Pattern = 's_user_enabled'; Message = 'Status LED needs a persistent user override.' },
    @{ Content = $led; Pattern = 'status_led_service_set_user_enabled'; Message = 'Status LED needs a public override API.' },
    @{ Content = $ui; Pattern = 'ui_service_request_exit_standby'; Message = 'UI must expose a task-safe standby-exit request.' },
    @{ Content = $cmake; Pattern = 'MID/system/voice_iot_command_service\.c'; Message = 'The IoT command service must be compiled.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

# Alarm cancellation: "取消10点的闹钟" must hit the cancel branch, never the
# set branch (which matches any 闹钟 text with a clock and would create a
# second alarm).  The cancel dispatch must come before set-alarm dispatch.
$iotCancelAt = $iot.IndexOf('timer_service_cancel_alarm')
$iotSetAt = $iot.IndexOf('timer_service_set_alarm')
if ($iotCancelAt -lt 0) { throw 'IoT must dispatch alarm cancellation through timer_service_cancel_alarm.' }
if ($iotSetAt -lt 0) { throw 'IoT set-alarm dispatch is missing.' }
if ($iotCancelAt -gt $iotSetAt) {
    throw 'Alarm cancel dispatch must precede set-alarm dispatch or cancel texts create a second alarm.'
}
if ($timer -notmatch 'timer_service_cancel_alarm\(uint8_t hour, uint8_t minute\)') {
    throw 'Timer service must expose the cancel-alarm API.'
}
if ($iot -notmatch '"取消"') { throw 'Alarm cancel must recognize the 取消 verb.' }

if (!$iot_hex.Contains('E5-85-B3-E9-97-AD-E7-81-AF-E5-B8-A6')) { throw 'IoT v1 must recognize the closed-light-strip command.' }
if (!$iot_hex.Contains('E6-89-93-E5-BC-80-E7-81-AF-E5-B8-A6')) { throw 'IoT v1 must recognize the open-light-strip command.' }
if (!$iot_hex.Contains('E9-80-80-E5-87-BA-E5-BE-85-E6-9C-BA')) { throw 'IoT v1 must recognize the exit-standby command.' }

$iotStart = $iot.IndexOf('voice_iot_command_service_execute')
if ($iotStart -lt 0) { throw 'IoT command dispatcher is missing.' }
$iotBlock = $iot.Substring($iotStart)
if ($iotBlock -match 'bms|stc|charging') {
    throw 'IoT v1 must not access unverified BMS, STC, or charging data.'
}

if ($voice -match 'local_iot_command|XIAOZHI_VOICE_IOT_WAIT_TTS') {
    throw 'IoT v1 must preserve the cloud natural-language reply and must not add a local TTS state.'
}

Write-Output 'Voice IoT command contract passed.'
