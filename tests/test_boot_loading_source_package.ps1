$ErrorActionPreference = 'Stop'

$package = Join-Path $PSScriptRoot '..\deliverables\boot_loading_animation'
$header = Get-Content -Raw -LiteralPath (Join-Path $package 'boot_loading.h')
$source = Get-Content -Raw -LiteralPath (Join-Path $package 'boot_loading.c')
$readme = Get-Content -Raw -LiteralPath (Join-Path $package 'README.md')

if ($header -notmatch 'BOOT_LOADING_DURATION_US' -or
    $source -notmatch 'boot_loading_create' -or
    $source -notmatch 'boot_loading_update' -or
    $source -notmatch 'boot_loading_is_complete') {
    throw 'The handoff package must expose a self-contained ten-second boot-loading API.'
}
if ($source -notmatch 'make_label\(view->root, "Starting"' -or
    $source -notmatch 'make_label\(view->root, "\.\.\."' -or
    $source -notmatch 'lv_label_set_text\(view->ellipsis, "\."\);' -or
    $source -notmatch 'lv_label_set_text\(view->ellipsis, ellipsis\[phase\]\);') {
    throw 'The handoff package must keep Starting fixed and animate only the dots.'
}
if ($source -notmatch 'lv_color_black\(\)' -or
    $source -notmatch 'lv_color_white\(\)' -or
    $source -notmatch 'lv_obj_move_foreground\(view->progress_frame\)' -or
    $source -notmatch 'BOOT_LOADING_FRAME_WIDTH \* progress') {
    throw 'The handoff package must retain the black screen and white framed loading bar.'
}
if ($source -match 'progress_value' -or $source -match 'snprintf') {
    throw 'The handoff package must not include a numeric progress label.'
}
if ($readme -notmatch '10' -or
    $readme -notmatch 'Starting' -or
    $readme -notmatch 'LVGL') {
    throw 'The handoff package must document duration, visible behavior, and integration dependency.'
}

Write-Output 'Boot loading source package checks passed.'
