$ErrorActionPreference = 'Stop'
$ui = Get-Content (Join-Path $PSScriptRoot '..\main\APP\ui_service.c') -Raw
if ($ui -match 'voice_status_text|voice_text_for_state') { throw 'XiaoZhi voice state text must be removed.' }
if ($ui -notmatch 'lv_obj_set_size\(s_scene\.voice_mouth, 44, mouth_h\)') { throw 'Voice mouth must be enlarged to 44px long.' }
if ($ui -notmatch 'lv_obj_set_pos\(s_scene\.voice_mouth, 138, 138 - mouth_h / 2\)') { throw 'Voice mouth must sit below the eyes with a visible gap.' }
Write-Output 'Voice face text and mouth contract passed.'
