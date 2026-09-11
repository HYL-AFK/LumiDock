$ErrorActionPreference = 'Stop'
$ui = Get-Content (Join-Path $PSScriptRoot '..\main\APP\ui_service.c') -Raw
$voice = Get-Content (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_voice_service.c') -Raw
if ($ui -notmatch 'ui_cjk_font_17') { throw 'Network page must use the native 17px CJK font.' }
if ($ui -match 'transform_scale') { throw 'Network text must not use geometric font scaling.' }
foreach ($needle in @('voice_mouth','voice_thinking_dots','VOICE_UI_TIMEOUT','VOICE_UI_CONNECTING','VOICE_UI_ERROR')) {
    if ($ui -notmatch $needle) { throw "Missing voice expression object/state: $needle" }
}
if ($ui -notmatch 'LV_OBJ_FLAG_HIDDEN.*voice_mouth|voice_mouth.*LV_OBJ_FLAG_HIDDEN') { throw 'Mouth must be hidden outside speaking state.' }
if ($ui -notmatch 'network_dot_count.*% 4U') { throw 'Thinking/network dots must cycle through 0..3.' }
if ($voice -notmatch 'prompt unavailable; playing local error tone then closing the session') { throw 'Prompt failures must answer with the local error tone before the session closes.' }
if ($voice -notmatch 'play_error_tone_then_close') { throw 'The play-error-tone-then-close helper is missing.' }
if ($voice -match 'XIAOZHI_TIMEOUT_UI_HOLD_US|s_timeout_ui_deadline_us|returning to main after 5s') { throw 'The timeout hold that bounced back to the main page must be removed.' }
if ($voice -notmatch 'cloud data acquisition timeout; requesting generated timeout prompt') { throw 'Cloud data timeout must request a cloud-spoken timeout prompt.' }
if ($voice -notmatch 'request_cloud_timeout_prompt') { throw 'Cloud data timeout must request a spoken timeout prompt from the cloud.' }
if ($voice -notmatch 'timeout prompt PCM drained; follow-up window=%lldms') { throw 'Timeout prompt must re-arm the follow-up window after playback.' }
Write-Output 'Voice expression v2 contract passed.'
