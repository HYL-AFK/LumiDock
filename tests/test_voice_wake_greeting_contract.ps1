$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

@(
    @{ Pattern = 'static uint32_t s_greeting_turn_id;'; Message = 'Wake detect needs a dedicated pending greeting turn ID.' },
    @{ Pattern = 's_greeting_turn_id ='; Message = 'Successful wake detect must register one pending greeting turn.' },
    @{ Pattern = 's_greeting_turn_id != 0U'; Message = 'TTS start must recognize the one pending wake greeting.' },
    @{ Pattern = 's_greeting_turn_id = 0U;'; Message = 'Accepted wake greeting must be consumed exactly once.' },
    @{ Pattern = 'wake greeting TTS accepted: turn=%u'; Message = 'Wake greeting acceptance must be visible in COM6.' }
) | ForEach-Object {
    if ($voice -notmatch $_.Pattern) { throw $_.Message }
}

$detectStart = $voice.IndexOf('static void send_detect(bool start_current_speech)')
$detectEnd = $voice.IndexOf('static void start_followup_listen(void)', $detectStart)
if ($detectStart -lt 0 -or $detectEnd -lt 0) {
    throw 'Wake detect helper boundary is missing.'
}
$detectBlock = $voice.Substring($detectStart, $detectEnd - $detectStart)
if ($detectBlock -notmatch 'send_text\(' -or $detectBlock -notmatch 's_greeting_turn_id =') {
    throw 'Wake greeting must be registered only after listen/detect is sent.'
}
if ($detectBlock -notmatch 'text\\":\\"你好,小智' -or $detectBlock -match '\[device_call\]') {
    throw 'Normal WakeNet greeting must use the known-good wake-word detect text, not direct-TTS.'
}

$ttsStart = $voice.IndexOf('strcmp(state->valuestring, "start") == 0')
$ttsStop = $voice.IndexOf('strcmp(state->valuestring, "stop") == 0', $ttsStart)
if ($ttsStart -lt 0 -or $ttsStop -lt 0) {
    throw 'TTS state handling boundaries are missing.'
}
$ttsBlock = $voice.Substring($ttsStart, $ttsStop - $ttsStart)
if ($ttsBlock -notmatch 'XIAOZHI_VOICE_WAIT_RESPONSE' -or $ttsBlock -notmatch 's_greeting_turn_id != 0U') {
    throw 'Only a real response or the one pending wake greeting may accept TTS start.'
}

$vadEndStart = $voice.IndexOf('case VOICE_EVENT_VAD_END:')
$vadEndEnd = $voice.IndexOf('case VOICE_EVENT_NETWORK_LOST:', $vadEndStart)
if ($vadEndStart -lt 0 -or $vadEndEnd -lt 0) {
    throw 'VAD-end state-machine boundary is missing.'
}
$vadEndBlock = $voice.Substring($vadEndStart, $vadEndEnd - $vadEndStart)
if ($vadEndBlock -notmatch 's_state == XIAOZHI_VOICE_SPEAKING') {
    throw 'Wake-word VAD end must not reset AFE after greeting TTS has started.'
}

Write-Output 'Wake greeting TTS contract passed.'
