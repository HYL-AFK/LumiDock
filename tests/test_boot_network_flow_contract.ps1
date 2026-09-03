$ErrorActionPreference = 'Stop'
$ui = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\main\APP\ui_service.c'), [System.Text.Encoding]::UTF8)
$startup = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\main\APP\startup_coordinator.c'), [System.Text.Encoding]::UTF8)
$networkText = ([char]0x8054).ToString() + ([char]0x7f51).ToString() + ([char]0x4e2d).ToString()

@(
    @{ Pattern = 'UI_BOOT_MIN_DURATION_US\s*\(10000000LL\)'; Message = 'Boot minimum duration must stay at ten seconds.' },
    @{ Pattern = 'UI_BOOT_MAX_DURATION_US\s*\(15000000LL\)'; Message = 'Boot maximum duration must stay at fifteen seconds.' },
    @{ Pattern = 'UI_BOOT_SOUND_LEAD_US\s*\(4000000LL\)'; Message = 'Boot sound lead must stay at four seconds.' },
    @{ Pattern = 'UI_PAGE_NETWORK'; Message = 'UI must have a dedicated network page.' },
    @{ Pattern = 'network_dot_count'; Message = 'Network page must animate a bounded dot count.' },
    @{ Pattern = 'UI_NETWORK_TEXT_WIDTH 160'; Message = 'Network text frame must have a fixed width of 160px.' },
    @{ Pattern = 'UI_NETWORK_TEXT_HEIGHT 40'; Message = 'Network text frame must have a fixed height of 40px.' },
    @{ Pattern = 'network_page_started_us'; Message = 'Network page must track its decision start time.' },
    @{ Pattern = 'UI_NETWORK_PAGE_DURATION_US\s*\(3LL\s*\*\s*1000LL\s*\*\s*1000LL\)'; Message = 'Network page must keep a three-second initial window.' },
    @{ Pattern = 'UI_NETWORK_CONNECT_GRACE_US\s*\(12LL\s*\*\s*1000LL\s*\*\s*1000LL\)'; Message = 'Network page must keep a twelve-second grace window.' },
    @{ Pattern = 'boot_transition_complete'; Message = 'Boot transition completion must remain the handoff point into the network decision.' },
    @{ Pattern = 'boot_cloud_ready'; Message = 'UI must expose a boot-cloud readiness gate for the greeting.' },
    @{ Pattern = 'voice_session\.connected'; Message = 'Network result must use XiaoZhi connection state.' },
    @{ Pattern = 'voice_session\.connection_failed'; Message = 'Network result must distinguish a failed XiaoZhi connection.' },
    @{ Pattern = 'mic_error'; Message = 'Main status bar must include the XiaoZhi failure microphone indicator.' },
    @{ Pattern = 'lv_color_hex\(0xD92D20\)'; Message = 'Microphone failure indicator must be red.' },
    @{ Pattern = 'network_label.*network_message'; Message = 'Connecting text must be created under the dedicated network page.' }
) | ForEach-Object { if ($ui -notmatch $_.Pattern) { throw $_.Message } }

if ($ui.IndexOf($networkText) -lt 0) {
    throw 'Network page must show the Chinese connecting text.'
}

if ($ui -match ('voice_page[^\r\n]*' + [regex]::Escape($networkText)) -or
    $ui -match ('voice_status_text[^\r\n]*' + [regex]::Escape($networkText))) {
    throw 'The XiaoZhi page must never render the network-page connecting text.'
}
if ($ui -notmatch 'lv_obj_add_flag\(s_scene\.network_label, LV_OBJ_FLAG_HIDDEN\)' -or
    $ui -notmatch 'lv_obj_add_flag\(s_scene\.network_dots, LV_OBJ_FLAG_HIDDEN\)') {
    throw 'Leaving the network page must explicitly hide its labels.'
}
$networkUpdateStart = $ui.IndexOf('static void update_network_page')
$networkUpdateEnd = $ui.IndexOf('static void apply_page_visibility', $networkUpdateStart)
if ($networkUpdateStart -lt 0 -or $networkUpdateEnd -lt 0) {
    throw 'Network page update function boundaries are missing.'
}
$networkUpdate = $ui.Substring($networkUpdateStart, $networkUpdateEnd - $networkUpdateStart)
if ($networkUpdate.IndexOf('elapsed_us < UI_NETWORK_PAGE_DURATION_US') -lt 0 -or
    $networkUpdate.IndexOf('elapsed_us < UI_NETWORK_PAGE_DURATION_US') -gt
    $networkUpdate.IndexOf('snapshot->voice_session.connected')) {
    throw 'Network page must hold the initial three-second window before accepting a connection.'
}
if ($ui -notmatch 'UI_NETWORK_PAGE_DURATION_US\s*\(3LL\s*\*\s*1000LL\s*\*\s*1000LL\)' -or
    $ui -notmatch 'UI_NETWORK_CONNECT_GRACE_US\s*\(12LL\s*\*\s*1000LL\s*\*\s*1000LL\)') {
    throw 'Network page total decision window must be fifteen seconds.'
}

if ($startup -notmatch 'ui_service_boot_cloud_ready\(\)') {
    throw 'Startup greeting must only start after the boot network decision confirms XiaoZhi is connected.'
}
if ($startup -notmatch 'ui_service_boot_transition_complete\(\)') {
    throw 'Startup greeting must still wait for the boot UI transition gate.'
}

# The boot fade-in must wait for the network page itself; the main page is not
# a visible or staging step during startup.
$transitionStart = $ui.IndexOf('static void update_boot_transition')
$transitionEnd = $ui.IndexOf('static bool charging_active', $transitionStart)
if ($transitionStart -lt 0 -or $transitionEnd -lt 0) {
    throw 'Boot transition function boundaries are missing.'
}
$transition = $ui.Substring($transitionStart, $transitionEnd - $transitionStart)
$networkHandoff = $transition.IndexOf('begin_network_decision(now_us)')
$networkWait = $transition.IndexOf('UI_BOOT_TRANSITION_WAIT_NETWORK_FRAME')
$fadeIn = $transition.IndexOf('st7789_fade_backlight_percent(100U')
if ($networkHandoff -lt 0 -or $networkWait -lt 0 -or $fadeIn -lt 0 -or $networkHandoff -gt $networkWait -or $networkWait -gt $fadeIn) {
    throw 'Boot transition must select and flush the network page before fading in.'
}
if ($ui -match 'start_main_page_from_boot|UI_BOOT_TRANSITION_WAIT_MAIN_FRAME|main_first_frame_flushed') {
    throw 'Boot transition must not depend on a main-page staging frame.'
}

$voice = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_voice_service.c'), [System.Text.Encoding]::UTF8)
$helloStart = $voice.IndexOf('strcmp(type->valuestring, "hello")')
$helloEnd = $voice.IndexOf('} else if (strcmp(type->valuestring, "tts")', $helloStart)
if ($helloStart -lt 0 -or $helloEnd -lt 0) {
    throw 'WSS hello handler boundaries are missing.'
}
$hello = $voice.Substring($helloStart, $helloEnd - $helloStart)
if ($hello -notmatch 's_hello_ready\s*=\s*true[\s\S]*?publish_ui_state\(') {
    throw 'WSS hello must publish the connected voice snapshot immediately.'
}

$font = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\main\APP\ui_cjk_font.c'), [System.Text.Encoding]::UTF8)
foreach ($glyph in @([char]0x8054, [char]0x7f51, [char]0x4e2d)) {
    $code = ('{0:X4}' -f [int][char]$glyph)
    if ($font -notmatch $code) {
        throw "The CJK subset font must contain glyph U+$code ($glyph)."
    }
}
Write-Output 'Boot network flow contract passed.'
