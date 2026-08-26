$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

@(
    @{ Pattern = '#include "mid_snapshot\.h"'; Message = 'Boot greeting must read the local time/weather snapshot.' },
    @{ Pattern = 'XIAOZHI_BOOT_GREETING_TEXT_MAX_BYTES 384U'; Message = 'Boot greeting text needs a bounded UTF-8 buffer.' },
    @{ Pattern = 'mid_snapshot_get\(&snapshot\)'; Message = 'Boot greeting must read a stable local snapshot.' },
    @{ Pattern = '\[device_call\]'; Message = 'The open-source test server requires the device_call direct-TTS prefix.' },
    @{ Pattern = 'snapshot\.time_weather\.local_time\.tm_yday'; Message = 'Greeting template selection must remain stable for one day.' },
    @{ Pattern = 'cJSON_AddStringToObject\(root, "type", "listen"\)'; Message = 'Boot greeting must construct a listen message using cJSON.' },
    @{ Pattern = 'cJSON_AddStringToObject\(root, "state", "detect"\)'; Message = 'Boot greeting must send the direct-TTS request through listen/detect.' },
    @{ Pattern = 'cJSON_AddStringToObject\(root, "text", greeting\)'; Message = 'Boot greeting must send its prepared local text.' },
    @{ Pattern = 'boot greeting text prepared:'; Message = 'The prepared boot greeting must be visible in COM6.' },
    @{ Pattern = 'cloud alert:'; Message = 'A public-cloud rejection must be logged immediately.' },
    @{ Pattern = 'boot greeting rejected by cloud'; Message = 'A rejected boot greeting must return to ordinary wake mode without waiting for timeout.' }
) | ForEach-Object {
    if ($voice -notmatch $_.Pattern) { throw $_.Message }
}

if ($voice -match 'public cloud wake-word fallback') {
    throw 'The legacy public-cloud wake-word fallback must not remain in the boot greeting path.'
}

$beginStart = $voice.IndexOf('static void begin_boot_greeting(void)')
$beginEnd = $voice.IndexOf('static bool send_terminal_uplink_sequence(void)', $beginStart)
if ($beginStart -lt 0 -or $beginEnd -lt 0) { throw 'Boot greeting helper boundary is missing.' }
$beginBlock = $voice.Substring($beginStart, $beginEnd - $beginStart)
if ($beginBlock -notmatch 'send_boot_greeting_detect\(\)') {
    throw 'Boot greeting must send through one owned helper.'
}

$normalDetectStart = $voice.IndexOf('static void send_detect(bool start_current_speech)')
$normalDetectEnd = $voice.IndexOf('static void start_followup_listen(void)', $normalDetectStart)
if ($normalDetectStart -lt 0 -or $normalDetectEnd -lt 0) { throw 'Normal wake detect helper boundary is missing.' }
$normalDetect = $voice.Substring($normalDetectStart, $normalDetectEnd - $normalDetectStart)
if ($normalDetect -notmatch 'text\\":\\"你好,小智' -or $normalDetect -match '\[device_call\]') {
    throw 'Normal wake detection must keep the measured short wake phrase and must not send device_call text.'
}

Write-Output 'Boot greeting local-text contract passed.'
