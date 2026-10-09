# 生成 UI 用的汉字子集，两个产物：
#   main/APP/ui_cjk_font.c        正文 14px（另挂一个 17px 描述符给网络页复用字形）
#   main/APP/ui_cjk_font_radio.c  电台那一行 20px（台名 | 状态，只收电台用得到的字）
#
# 为什么要有这个脚本：ui_cjk_font.c 除了 lv_font_conv 的产物，末尾还手工挂着一个
# 17px 的描述符（ui_cjk_font_17，网络页用，复用同一张字形表）。照抄文件头那行命令
# 直接重新生成会把那个描述符冲掉——构建不报错，网络页却整个没了字体。这里生成完
# 自动补回去，所以重生成请走这个脚本，别再手敲命令。
#
# 改 UI 文案时：把新用到的汉字补进 $symbols，跑一次，再跑
# tests/test_ui_reference_contract.ps1 —— 它会逐字核对界面字符串都有字形。
$ErrorActionPreference = 'Stop'

$symbols = '星期日一二三四五六月晴云雾雨雪雷小智激活码控制台输入后自动连接联网中天气暂不可用聆听思考讲话异常待机国之声经济音乐文艺播放已停'
if ($symbols.Length -ne 63) {
    throw "字形列表长度不对：应当 63 个字符，实际 $($symbols.Length) 个。漏一个字就是界面上的一个方框，而那要到实机才看得出来。"
}

# 电台那行要用的字：四个台名（中国之声/经济之声/音乐之声/文艺之声）+ 三种状态
# （播放中/已暂停/连接中）+ 分隔符 " | "。改台名或改状态文案时同步改这里，
# tests/test_ui_reference_contract.ps1 会拿实际文案逐字核对。
$radioSymbols = ' |中国之声经济音乐文艺播放已暂停连接'
if ($radioSymbols.Length -ne 19) {
    throw "电台字形列表长度不对：应当 19 个字符，实际 $($radioSymbols.Length) 个。漏一个字就是台名或状态上的一个方框。"
}

$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root 'main\APP\ui_cjk_font.c'
$staged = Join-Path $root 'tmp\ui_cjk_font_generated.c'

Push-Location $root
try {
    & npx --yes lv_font_conv --size 14 --bpp 4 --format lvgl --lv-include lvgl.h `
        --font managed_components\lvgl__lvgl\scripts\built_in_font\SourceHanSansSC-Normal.otf `
        -r 0x20-0x7f --symbols $symbols --lv-font-name ui_cjk_font_14 `
        -o tmp\ui_cjk_font_generated.c --force-fast-kern-format
    if ($LASTEXITCODE -ne 0) { throw "lv_font_conv 失败，退出码 $LASTEXITCODE" }

    # 电台那一行（“台名 | 状态”）用的大字号。正文 14px 在 320x170 上只有 14 个像素
    # 高，而这一行是用户盯着看的东西（哪个台、在不在放），单独给它一份 20px。
    # 只收电台用得到的字：四个台名 + 三种状态 + 那个竖线分隔符，比再生成一份完整
    # 正文子集便宜得多（二进制约 4 KB）。
    & npx --yes lv_font_conv --size 20 --bpp 4 --format lvgl --lv-include lvgl.h `
        --font managed_components\lvgl__lvgl\scripts\built_in_font\SourceHanSansSC-Normal.otf `
        --symbols $radioSymbols --lv-font-name ui_cjk_font_radio `
        -o main\APP\ui_cjk_font_radio.c --force-fast-kern-format
    if ($LASTEXITCODE -ne 0) { throw "电台字体生成失败，退出码 $LASTEXITCODE" }
}
finally {
    Pop-Location
}

$generated = Get-Content -LiteralPath $staged -Raw -Encoding utf8
$tail = $generated.LastIndexOf('#endif /*#if UI_CJK_FONT_14*/')
if ($tail -lt 0) { throw '生成的字体文件结构不对：找不到收尾的 #endif。' }

$face = @'
/* Native layout face for the 17px network slot.  It reuses the compact CJK
 * glyph table, but has its own descriptor so no transform scaling is applied. */
const lv_font_t ui_cjk_font_17 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 20,
    .base_line = 4,
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -2,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



'@

Set-Content -LiteralPath $target -Value ($generated.Substring(0, $tail) + $face + $generated.Substring($tail)) `
    -Encoding utf8NoBOM -NoNewline
Write-Output "ui_cjk_font.c regenerated with $($symbols.Length) CJK symbols (17px face re-attached)."
