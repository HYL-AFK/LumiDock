$ErrorActionPreference = 'Stop'
$ui = Get-Content (Join-Path $PSScriptRoot '..\main\APP\ui_service.c') -Raw
foreach ($needle in @(
    'lv_obj_set_style_bg_opa\(s_scene\.voice_mouth, LV_OPA_COVER, 0\)',
    'lv_obj_set_style_bg_opa\(s_scene\.voice_thinking_dots\[i\], LV_OPA_COVER, 0\)',
    'VOICE_UI_THINKING',
    'VOICE_UI_SPEAKING')) {
    if ($ui -notmatch $needle) { throw "Missing visible face object contract: $needle" }
}
if ($ui -notmatch 'const uint32_t phase = s_scene\.voice_pulse_frame % 8U') { throw 'Connecting eyes must use a compact eight-step oscillation.' }
if ($ui -notmatch 'phase <= 4U \? phase : 8U - phase') { throw 'Connecting eyes must sway left and right symmetrically.' }
if ($ui -match 'return "获取超时"') { throw 'Timeout label must not use glyphs absent from the compact CJK font.' }
if ($ui -match 'voice_status_text|voice_text_for_state') { throw 'XiaoZhi voice state text must remain removed.' }
Write-Output 'UI face visibility contract passed.'
