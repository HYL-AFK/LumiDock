#pragma once

#include "lvgl.h"

/* 只保留实际在用的：14px 正文/二级文本、17px 网络页、34px 粗体日期。
   原来的 24 / 34 / 48px 三个字体全项目零引用，已连同生成文件一起删除。 */
LV_FONT_DECLARE(ui_cjk_font_14);
LV_FONT_DECLARE(ui_cjk_font_17);
LV_FONT_DECLARE(ui_cjk_font_34_black);
