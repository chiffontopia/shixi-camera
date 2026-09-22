/*
 * icontest.c — 主机端图标预览（把应用图标与所有小图标排成一张图，便于调整美术）
 * 用法: ./bin/icontest out.png
 */
#include "gfx.h"
#include "ui.h"
#include "font.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "icons.png";
    if (gfx_init() != 0 || font_init() != 0) return 1;
    Surface *s = gfx_back();
    gfx_clear(s, C_BG);

    /* 应用图标 */
    const char *names[APPICON_COUNT] = { "拍照", "录像", "图库", "AI聊天" };
    for (int i = 0; i < APPICON_COUNT; i++) {
        Surface *ic = appicon_get((AppIconId)i, 112);
        int x = 40 + i * 180, y = 40;
        if (ic) {
            gfx_blit_keyed(s, x, y, ic);
            fprintf(stderr, "appicon %d %s: %dx%d px[0]=%08x px[c]=%08x\n", i, names[i],
                    ic->w, ic->h, ic->px[0], ic->px[56 * ic->stride + 56]);
        } else {
            fprintf(stderr, "appicon %d %s: NULL\n", i, names[i]);
        }
        text_draw(s, x, y + 120, names[i], FONT_BODY, C_TEXT);
    }

    /* 小图标 */
    const char *inames[IC_COUNT] = {
        "BACK", "TRASH", "PLAY", "PAUSE", "SEND", "CHECK", "CLOSE",
        "FLASH", "FLIP", "SETTINGS", "GRID", "MORE", "CHEV_L", "CHEV_R",
        "INFO", "SDCARD", "SIGNAL", "BATTERY", "HOME", "IMAGE", "MOVIE",
        "SEARCH", "CLOCK", "SUN", "ZOOM", "LOCK", "REFRESH"
    };
    for (int i = 0; i < IC_COUNT; i++) {
        int col = i % 9, row = i / 9;
        int x = 60 + col * 80, y = 220 + row * 80;
        icon_draw(s, (IconId)i, x, y, 36, C_TEXT);
        text_draw(s, x - 24, y + 26, inames[i], FONT_SMALL, C_TEXT_DIM);
    }

    /* 文字样本 */
    text_draw(s, 40, 400, "文字测试 拍照 录像 图库 删除 12:34:56 87%", FONT_BODY, C_TEXT);
    text_draw(s, 40, 424, "特大字 09:33", FONT_HUGE, C_ACCENT);
    text_draw(s, 40, 470 - 20, "小字提示：轻触图标进入应用 · 左右滑动翻页", FONT_SMALL, C_TEXT_DIM);

    int ret = gfx_save_ppm(out);
    fprintf(stderr, "saved %s: %d\n", out, ret);
    gfx_shutdown();
    return 0;
}
