$ErrorActionPreference = 'Stop'

$root = Join-Path $PSScriptRoot '..'
$voice = [System.IO.File]::ReadAllText((Join-Path $root 'main\MID\voice\xiaozhi_voice_service.c'), [System.Text.Encoding]::UTF8)
$generator = [System.IO.File]::ReadAllText((Join-Path $root 'scripts\generate_prompt_tones.py'), [System.Text.Encoding]::UTF8)
$cmake = [System.IO.File]::ReadAllText((Join-Path $root 'main\CMakeLists.txt'), [System.Text.Encoding]::UTF8)

@(
    @{ Pattern = '#include "mid_snapshot\.h"'; Message = 'Boot greeting must read the live time/weather snapshot.' },
    @{ Pattern = 'mid_snapshot_get\(&snapshot\)'; Message = 'Boot greeting must read a stable local snapshot.' },
    @{ Pattern = 'static size_t select_boot_greeting_segments'; Message = 'Boot greeting must assemble its pre-rendered segments from the live clock and weather.' },
    @{ Pattern = 'boot greeting PCM bucket='; Message = 'The chosen greeting bucket must be visible in COM6.' },
    @{ Pattern = 'boot greeting local PCM playing:'; Message = 'The playing greeting asset must be visible in COM6.' },
    @{ Pattern = 'cloud alert:'; Message = 'A public-cloud rejection must be logged immediately.' },
    @{ Pattern = 'boot greeting rejected by cloud'; Message = 'A rejected boot greeting must return to ordinary wake mode without waiting for timeout.' }
) | ForEach-Object {
    if ($voice -notmatch $_.Pattern) { throw $_.Message }
}

if ($voice -match 'public cloud wake-word fallback') {
    throw 'The legacy public-cloud wake-word fallback must not remain in the boot greeting path.'
}

# The official xiaozhi.me server rejects the [device_call] direct-TTS prefix, so
# the cloud greeting text path must be gone and no greeting text may be sent.
if ($voice -match '"\[device_call\]') {
    throw 'The boot greeting must not send a [device_call] direct-TTS request.'
}
if ($voice -match 'send_boot_greeting_detect|build_boot_greeting_text|XIAOZHI_BOOT_GREETING_TEXT_MAX_BYTES') {
    throw 'The legacy cloud boot-greeting text path must be gone.'
}

# The greeting is spoken as four concatenated segments
# ([时段问候] + [现在是X点] + [天气描述] + [有什么我可以帮你的吗？]).  Every segment must be
# produced by the tone generator and compiled into the firmware image.
$assets = @()
foreach ($period in @('forenoon', 'noon', 'afternoon', 'evening', 'night', 'generic')) {
    $assets += "prompt_greet_${period}_16k_mono_s16le.pcm"
}
foreach ($hour in 0..23) {
    $assets += ('prompt_hour_{0:d2}_16k_mono_s16le.pcm' -f $hour)
}
foreach ($condition in @('clear', 'cloudy', 'fog', 'rain', 'snow', 'thunder', 'unknown')) {
    $assets += "prompt_wx_${condition}_16k_mono_s16le.pcm"
}
# The help offer has no variants, so it is asserted as one literal asset rather
# than a table.
$assets += 'prompt_greet_closing_16k_mono_s16le.pcm'
foreach ($asset in $assets) {
    $symbol = $asset -replace '_16k_mono_s16le\.pcm$', ''
    if ($voice.IndexOf("DECLARE_EMBEDDED_PCM($symbol);") -lt 0) {
        throw "Boot greeting is missing the '$asset' asset reference."
    }
    if ($cmake.IndexOf("assets/audio/$asset") -lt 0) {
        throw "The '$asset' greeting asset must be embedded into the firmware image."
    }
    if ($generator.IndexOf($asset) -lt 0) {
        throw "The '$asset' greeting asset must be produced by the tone generator."
    }
}
if ($voice.IndexOf('"_binary_" #symbol "_16k_mono_s16le_pcm_start"') -lt 0) {
    throw 'DECLARE_EMBEDDED_PCM must expand to the _binary_ symbol the build emits.'
}

# The spoken clock is indexed by the live hour and every clip carries its own
# time modifier, so 07:00 and 19:00 can never collapse into one ambiguous
# sentence.
if ($voice -notmatch 'static boot_period_t boot_period_from_hour\(int hour\)') {
    throw 'The period greeting must be derived from the live hour.'
}
if ($voice -notmatch 'segments\[count\]\.start = s_boot_hour_pcm\[hour\]\.start;') {
    throw 'The boot greeting must index the spoken clock table with the live hour.'
}
$weatherStart = $voice.IndexOf('static boot_weather_t boot_weather_from_text(const char *weather)')
$weatherEnd = $voice.IndexOf('static size_t select_boot_greeting_segments', $weatherStart)
if ($weatherStart -lt 0 -or $weatherEnd -lt 0) { throw 'Weather bucket helper boundary is missing.' }
$weatherBlock = $voice.Substring($weatherStart, $weatherEnd - $weatherStart)
if ($weatherBlock -notmatch 'strstr\(weather, "晴"\)[\s\S]{0,80}return BOOT_WEATHER_UNKNOWN;') {
    throw 'Only an explicit clear-weather code may select the clear-weather copy.'
}

$beginStart = $voice.IndexOf('static void begin_boot_greeting(void)')
$beginEnd = $voice.IndexOf('static bool send_terminal_uplink_sequence(void)', $beginStart)
if ($beginStart -lt 0 -or $beginEnd -lt 0) { throw 'Boot greeting helper boundary is missing.' }
$beginBlock = $voice.Substring($beginStart, $beginEnd - $beginStart)
if ($beginBlock -notmatch 'select_boot_greeting_segments\(greeting_segments,') {
    throw 'Boot greeting must assemble its segments through one owned helper.'
}
if ($beginBlock -notmatch 'play_local_prompt_segments\(LOCAL_PROMPT_BOOT_GREETING, greeting_segments,') {
    throw 'Boot greeting must play its embedded segments through the shared local-prompt playout.'
}
if ($beginBlock -match 'send_text\(') {
    throw 'Boot greeting must not send any cloud text request.'
}

$normalDetectStart = $voice.IndexOf('static void send_detect(bool start_current_speech)')
$normalDetectEnd = $voice.IndexOf('static void start_followup_listen(void)', $normalDetectStart)
if ($normalDetectStart -lt 0 -or $normalDetectEnd -lt 0) { throw 'Normal wake detect helper boundary is missing.' }
$normalDetect = $voice.Substring($normalDetectStart, $normalDetectEnd - $normalDetectStart)
if ($normalDetect -notmatch 'text\\":\\"你好,小智' -or $normalDetect -match '\[device_call\]') {
    throw 'Normal wake detection must keep the measured short wake phrase and must not send device_call text.'
}

Write-Output 'Boot greeting local-PCM contract passed.'
