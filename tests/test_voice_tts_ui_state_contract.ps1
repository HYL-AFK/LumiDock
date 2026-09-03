$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

$ttsStart = $voice.IndexOf('if (cJSON_IsString(state) && strcmp(state->valuestring, "start") == 0)')
$ttsStop = $voice.IndexOf('} else if (cJSON_IsString(state) && strcmp(state->valuestring, "stop") == 0)', $ttsStart)
if ($ttsStart -lt 0 -or $ttsStop -lt 0) { throw 'TTS start state handler boundaries are missing.' }
$ttsStartBlock = $voice.Substring($ttsStart, $ttsStop - $ttsStart)

if ($ttsStartBlock -notmatch 'const bool response_turn =') {
    throw 'TTS start must distinguish a real response turn from greeting playback.'
}
if ($ttsStartBlock -notmatch 'if \(response_turn\)\s*\{[\s\S]*?publish_ui_state\(VOICE_UI_THINKING\);') {
    throw 'Only a real cloud response may publish THINKING.'
}
if ($ttsStartBlock -notmatch 'if \(response_turn\)\s*\{\s*publish_ui_state\(VOICE_UI_THINKING\);\s*\}') {
    throw 'Boot and wake greeting TTS must not publish THINKING outside the response_turn guard.'
}

$playbackStart = $voice.IndexOf('case VOICE_EVENT_TTS_PLAYBACK_STARTED:')
$playbackEnd = $voice.IndexOf('case VOICE_EVENT_TTS_PLAYBACK_FINISHED:', $playbackStart)
if ($playbackStart -lt 0 -or $playbackEnd -lt 0) { throw 'TTS playback start event boundaries are missing.' }
$playbackBlock = $voice.Substring($playbackStart, $playbackEnd - $playbackStart)
if ($playbackBlock -notmatch 'publish_ui_state\(VOICE_UI_SPEAKING\);') {
    throw 'TTS playback start must publish SPEAKING.'
}

Write-Output 'Voice TTS UI state contract passed.'
