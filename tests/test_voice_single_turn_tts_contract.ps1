$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

@(
    @{ Pattern = 'XIAOZHI_VOICE_WAIT_RESPONSE'; Message = 'A real uplink must have an explicit wait-for-response state.' },
    @{ Pattern = 'static uint32_t s_turn_id;'; Message = 'Voice service must identify each local question turn.' },
    @{ Pattern = 'static uint32_t s_waiting_turn_id;'; Message = 'Voice service must track which local turn is awaiting cloud TTS.' },
    @{ Pattern = '\+\+s_turn_id;'; Message = 'Starting a real listen turn must allocate a new local turn ID.' },
    @{ Pattern = 's_state = XIAOZHI_VOICE_WAIT_RESPONSE;'; Message = 'listen/stop must transition to wait-for-response, not wake idle.' },
    @{ Pattern = 'duplicate/stale TTS start'; Message = 'Unexpected duplicate TTS starts must be visible in COM6 logs.' },
    @{ Pattern = 'static bool s_discard_tts_stream;'; Message = 'A rejected TTS stream must suppress its following binary packets until stop.' },
    @{ Pattern = 's_discard_tts_stream = true;'; Message = 'Duplicate TTS start must invalidate its downlink stream.' },
    @{ Pattern = 's_discard_tts_stream = false;'; Message = 'TTS stop must clear the rejected-stream marker.' },
    @{ Pattern = 'const bool response_turn = s_state == XIAOZHI_VOICE_WAIT_RESPONSE'; Message = 'A completed real uplink must remain an accepted TTS source.' },
    @{ Pattern = 'const bool greeting_turn = s_state == XIAOZHI_VOICE_WAIT_SPEECH'; Message = 'The one registered wake greeting must be an explicit accepted TTS source.' },
    @{ Pattern = 'const bool boot_greeting_turn = s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS'; Message = 'The one boot greeting must be a distinct accepted TTS source.' },
    @{ Pattern = 'const bool timeout_turn = s_state == XIAOZHI_VOICE_EXITING'; Message = 'The generated timeout prompt must be an explicit accepted TTS source.' },
    @{ Pattern = 'const bool fallback_turn = s_state == XIAOZHI_VOICE_EXITING'; Message = 'The generated fallback prompt must be an explicit accepted TTS source.' },
    @{ Pattern = 'late TTS packet discarded outside accepted speaking turn'; Message = 'Binary TTS packets outside the accepted turn must be discarded.' },
    @{ Pattern = 'voice turn abort: id=%u'; Message = 'Wake-word barge-in must invalidate the current local turn.' }
) | ForEach-Object {
    if ($voice -notmatch $_.Pattern) { throw $_.Message }
}

$ttsStart = $voice.IndexOf('strcmp(state->valuestring, "start") == 0')
$ttsStop = $voice.IndexOf('strcmp(state->valuestring, "stop") == 0', $ttsStart)
if ($ttsStart -lt 0 -or $ttsStop -lt 0) {
    throw 'TTS state handling boundaries are missing.'
}
$ttsStartBlock = $voice.Substring($ttsStart, $ttsStop - $ttsStart)
if ($ttsStartBlock -notmatch 'if \(!response_turn && !greeting_turn && !boot_greeting_turn && !exit_turn &&[\s\S]*?!fallback_turn\)' -or
    $ttsStartBlock -notmatch 's_discard_tts_stream = true;' -or
    $ttsStartBlock -notmatch 'return;') {
    throw 'Only a real response, one wake greeting, one boot greeting, or one generated prompt may accept TTS start; all other starts must return.'
}

$binaryStart = $voice.IndexOf('case VOICE_EVENT_WSS_BINARY:')
$binaryEnd = $voice.IndexOf('    }', $binaryStart)
$binaryBlock = $voice.Substring($binaryStart, $binaryEnd - $binaryStart)
if ($binaryBlock -notmatch 's_discard_tts_stream') {
    throw 'Rejected TTS streams must not enqueue their following binary packets.'
}

Write-Output 'Single-turn TTS contract passed.'
