$ErrorActionPreference = 'Stop'
$ui = Get-Content (Join-Path $PSScriptRoot '..\main\APP\ui_service.c') -Raw
foreach ($pattern in @(
    'UI_NETWORK_TEXT_WIDTH 136',
    'UI_NETWORK_TEXT_HEIGHT 40',
    'UI_NETWORK_TEXT_LABEL_WIDTH 44',
    'UI_NETWORK_DOTS_LABEL_WIDTH 24',
    'lv_obj_set_style_transform_scale\(s_scene\.network_label, 512',
    'lv_obj_set_style_transform_scale\(s_scene\.network_dots, 512',
    'UI_VOICE_LISTEN_EYE_WIDTH 86',
    'UI_VOICE_THINK_EYE_WIDTH 76',
    'UI_VOICE_SPEAK_EYE_WIDTH 86',
    'VOICE_UI_THINKING[\s\S]*?UI_VOICE_THINK_EYE_HEIGHT',
    'VOICE_UI_SPEAKING[\s\S]*?UI_VOICE_SPEAK_EYE_HEIGHT')) {
    if ($ui -notmatch $pattern) { throw "Missing UI expression/network sizing contract: $pattern" }
}
if ($ui -match 'VOICE_UI_IDLE: return "休眠"') { throw 'Voice page must not show the standby label.' }
if ($ui -match 'VOICE_UI_IDLE: return "休眠"') { throw 'Voice page must not show the standby label.' }
if ($ui -notmatch 'ui_cjk_font_17') { throw 'Network page must use the named native font resource.' }
Write-Output 'Voice expression and network text contract passed.'
