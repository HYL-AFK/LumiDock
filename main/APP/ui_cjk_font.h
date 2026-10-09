#pragma once

#include "lvgl.h"

/* 只保留实际在用的：14px 正文/二级文本、17px 网络页、20px 电台那一行、34px 粗体日期。
   原来的 24 / 34 / 48px 三个字体全项目零引用，已连同生成文件一起删除。

   17px 不是一份独立的字形表：它和 14px 共用同一张位图，只是行距不同（见
   ui_cjk_font.c 末尾）。真要更大的字形就得新生成一份，ui_cjk_font_radio 就是
   这么来的——20px、只收电台那行用得到的 19 个字形。 */
LV_FONT_DECLARE(ui_cjk_font_14);
LV_FONT_DECLARE(ui_cjk_font_17);
LV_FONT_DECLARE(ui_cjk_font_radio);
LV_FONT_DECLARE(ui_cjk_font_34_black);
