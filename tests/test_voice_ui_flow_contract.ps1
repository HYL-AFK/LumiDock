$ErrorActionPreference = 'Stop'

$snapshotHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\mid_snapshot.h"
$snapshotSource = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\mid_snapshot.c"
$activation = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_activation_service.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"
$assetsHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_assets.h"
$assets = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_assets.c"
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"

@(
    @{ Content = $snapshotHeader; Pattern = 'voice_session_snapshot_t'; Message = 'The snapshot contract must expose the voice UI state.' },
    @{ Content = $snapshotHeader; Pattern = 'VOICE_UI_CONNECTING'; Message = 'Voice UI must distinguish cloud connection from listening.' },
    @{ Content = $snapshotHeader; Pattern = 'VOICE_UI_LISTENING'; Message = 'Voice UI must expose listening.' },
    @{ Content = $snapshotHeader; Pattern = 'VOICE_UI_THINKING'; Message = 'Voice UI must expose cloud thinking.' },
    @{ Content = $snapshotHeader; Pattern = 'VOICE_UI_SPEAKING'; Message = 'Voice UI must expose TTS playback.' },
    @{ Content = $snapshotHeader; Pattern = 'VOICE_UI_ERROR'; Message = 'Voice UI must expose recoverable errors.' },
    @{ Content = $snapshotSource; Pattern = 'mid_snapshot_set_voice_session'; Message = 'Voice service must publish its state through the snapshot.' },
    @{ Content = $voice; Pattern = 'publish_ui_state'; Message = 'The WSS state machine must publish each transition for the UI.' },
    @{ Content = $voice; Pattern = 'VOICE_UI_ERROR'; Message = 'Cloud failures must enter a recoverable UI error state.' },
    @{ Content = $activation; Pattern = 'restart_required'; Message = 'Only the first successful binding must request a reboot.' },
    @{ Content = $activation; Pattern = 'activation_code_issued'; Message = 'A reboot must be tied to a prior server-issued activation code.' },
    @{ Content = $ui; Pattern = 'UI_PAGE_VOICE'; Message = 'The UI must provide a dedicated voice page.' },
    @{ Content = $ui; Pattern = 'create_voice_page'; Message = 'Voice page construction must be separate from the main page.' },
    @{ Content = $ui; Pattern = 'update_voice_page'; Message = 'Voice page updates must use a snapshot state, not audio callbacks.' },
    @{ Content = $ui; Pattern = 'restart_required'; Message = 'Activation confirmation must be consumed by the UI state machine.' },
    @{ Content = $assetsHeader; Pattern = 'ui_voice_connecting'; Message = 'A compact connecting expression asset is required.' },
    @{ Content = $assetsHeader; Pattern = 'ui_voice_listening'; Message = 'A compact listening expression asset is required.' },
    @{ Content = $assetsHeader; Pattern = 'ui_voice_thinking'; Message = 'A compact thinking expression asset is required.' },
    @{ Content = $assetsHeader; Pattern = 'ui_voice_speaking'; Message = 'A compact speaking expression asset is required.' },
    @{ Content = $assetsHeader; Pattern = 'ui_voice_error'; Message = 'A compact error expression asset is required.' },
    @{ Content = $assets; Pattern = 'UI_IMAGE_I4'; Message = 'Voice expressions must use 4-bit indexed images, not RGB565.' },
    @{ Content = $cmake; Pattern = 'assets/ui/voice_connecting\.i4'; Message = 'Voice expression assets must be Flash-backed embedded files.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

if ($ui -match 'case VOICE_UI_IDLE:\s*default:\s*return "休眠"') {
    throw 'The XiaoZhi page must not label its idle state as sleep.'
}
if ($ui -match 'voice_status_text|voice_text_for_state|lv_label_set_text\(s_scene\.voice_status_text') {
    throw 'The XiaoZhi page must not render voice state text.'
}
if ($ui -notmatch 'UI_VOICE_LISTEN_EYE_WIDTH' -or $ui -notmatch 'UI_VOICE_SPEAK_EYE_WIDTH') {
    throw 'The XiaoZhi voice states must use explicit awake eye geometry.'
}
if ($ui -notmatch 'VOICE_UI_THINKING[\s\S]*?UI_VOICE_THINK_EYE_HEIGHT') {
    throw 'The XiaoZhi thinking state must use distinct narrow eyes.'
}

if ($ui -match 'xQueueReceive\(' -or $ui -match 'xiaozhi_voice_service_') {
    throw 'The UI must consume voice state only through the protected snapshot.'
}

Write-Output 'Voice UI flow contract passed.'
