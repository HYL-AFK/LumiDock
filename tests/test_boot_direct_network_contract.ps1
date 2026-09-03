$ErrorActionPreference = 'Stop'
$ui = Get-Content (Join-Path $PSScriptRoot '..\main\APP\ui_service.c') -Raw

if ($ui -match 'UI_BOOT_TRANSITION_WAIT_MAIN_FRAME|start_main_page_from_boot') {
    throw 'Boot transition must not use the main page as a staging step.'
}
if ($ui -notmatch 'UI_BOOT_TRANSITION_WAIT_NETWORK_FRAME') {
    throw 'Boot transition must wait for the network page first frame.'
}
$transition = $ui.Substring($ui.IndexOf('static void update_boot_transition'))
if ($transition -notmatch 'begin_network_decision\(now_us\)[\s\S]*?UI_BOOT_TRANSITION_WAIT_NETWORK_FRAME') {
    throw 'Animation handoff must select the network page before waiting for its frame.'
}
if ($transition -notmatch 'network_first_frame_flushed[\s\S]*?st7789_fade_backlight_percent') {
    throw 'Backlight fade-in must wait for the network first frame.'
}
Write-Output 'Direct network boot contract passed.'
