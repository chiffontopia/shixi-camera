/*
 * ui.c — 控件与图标实现
 *
 * 图标全部用图元现画（矢量风格），并用离屏画布缓存，避免每帧重复计算。
 */
#include "ui.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ================================================================== */
/* 图标：以 (cx,cy) 为中心、size 为边长绘制                              */
/* ================================================================== */
static void rounded_bars(Surface *s, int cx, int cy, int size, uint32_t c)
{
    /* 三条圆角竖条（信号） */
    int h = size, w = size / 6;
    int gap = size / 4;
    for (int i = 0; i < 3; i++) {
        int bh = h - i * gap;
        gfx_fill_round_rect(s, cx - size / 2 + i * (w + 2), cy + h / 2 - bh, w, bh, w / 2, c);
    }
}

void icon_draw(Surface *s, IconId id, int cx, int cy, int size, uint32_t c)
{
    if (!s) return;
    int h = size / 2;
    switch (id) {
    case IC_BACK: {
        int t = imax(2, size / 7);
        for (int i = 0; i < size / 2; i++) {
            gfx_fill_rect(s, cx - h / 2 + i, cy - i, t, 1, c);
            gfx_fill_rect(s, cx - h / 2 + i, cy + i, t, 1, c);
        }
        gfx_fill_rect(s, cx - h / 2, cy - t / 2, size / 2, t, c);
        break;
    }
    case IC_CHEVRON_L:
    case IC_CHEVRON_R: {
        int t = imax(2, size / 7);
        int dir = (id == IC_CHEVRON_R) ? 1 : -1;
        for (int i = 0; i < size / 2; i++) {
            gfx_fill_rect(s, cx - dir * (h / 2 - i), cy - i, t, 1, c);
            gfx_fill_rect(s, cx - dir * (h / 2 - i), cy + i, t, 1, c);
        }
        break;
    }
    case IC_TRASH: {
        int w = size * 3 / 4, bh = size * 3 / 5;
        int x = cx - w / 2, y = cy - size / 2 + size / 5;
        gfx_fill_round_rect(s, cx - size / 2, y - imax(2, size / 9), size, imax(2, size / 9), 1, c);
        gfx_fill_rect(s, cx - size / 10, y - size / 6, size / 5, size / 8, c);
        /* 桶身：外轮廓 + 内部镂空 */
        gfx_fill_round_rect(s, x, y, w, bh, imax(2, size / 12), c);
        gfx_fill_round_rect(s, x + imax(2, size / 12), y + imax(2, size / 10),
                            w - 2 * imax(2, size / 12), bh - imax(2, size / 7),
                            imax(1, size / 16), 0x000000);
        break;
    }
    case IC_PLAY: {
        gfx_fill_triangle(s, cx - h / 2, cy - h, cx - h / 2, cy + h, cx + h * 3 / 4, cy, c);
        break;
    }
    case IC_PAUSE: {
        int bw = imax(3, size / 5);
        gfx_fill_round_rect(s, cx - bw - bw / 4, cy - h, bw, size, bw / 3, c);
        gfx_fill_round_rect(s, cx + bw / 4, cy - h, bw, size, bw / 3, c);
        break;
    }
    case IC_SEND: {
        /* 纸飞机 */
        gfx_fill_triangle(s, cx - h, cy, cx + h, cy - h, cx + h, cy + h, c);
        gfx_fill_triangle(s, cx - h, cy, cx + h, cy - h, cx + h / 3, cy + h / 6, c);
        break;
    }
    case IC_CHECK: {
        int t = imax(2, size / 7);
        for (int i = 0; i < size / 3; i++) gfx_fill_rect(s, cx - h / 2 + i, cy + i / 2, t, t, c);
        for (int i = 0; i < size * 2 / 3; i++) gfx_fill_rect(s, cx - h / 2 + size / 3 + i, cy + size / 6 - i, t, t, c);
        break;
    }
    case IC_CLOSE: {
        int t = imax(2, size / 8);
        for (int i = -h; i <= h; i++) {
            gfx_fill_rect(s, cx + i, cy + i, t, t, c);
            gfx_fill_rect(s, cx + i, cy - i, t, t, c);
        }
        break;
    }
    case IC_FLASH: {
        /* 闪电 */
        gfx_fill_triangle(s, cx, cy - h, cx - h / 2, cy + h / 6, cx + h / 2, cy + h / 6, c);
        gfx_fill_triangle(s, cx - h / 3, cy - h / 6, cx + h / 5, cy - h / 6, cx - h / 6, cy + h, c);
        break;
    }
    case IC_FLIP: {
        gfx_circle_outline(s, cx, cy, h, imax(2, size / 8), c);
        gfx_fill_triangle(s, cx + h / 2, cy - h, cx + h + h / 3, cy - h / 3, cx + h / 2, cy - h / 3, c);
        break;
    }
    case IC_SETTINGS: {
        gfx_circle_outline(s, cx, cy, h, imax(2, size / 6), c);
        for (int i = 0; i < 8; i++) {
            static const int dx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
            static const int dy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
            gfx_fill_rect(s, cx + dx[i] * h - size / 12, cy + dy[i] * h - size / 12,
                          size / 6, size / 6, c);
        }
        break;
    }
    case IC_GRID: {
        int cell = size * 2 / 5, gap = size / 8;
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                gfx_fill_round_rect(s, cx - cell - gap / 2 + i * (cell + gap),
                                    cy - cell - gap / 2 + j * (cell + gap),
                                    cell, cell, imax(2, size / 12), c);
        break;
    }
    case IC_MORE: {
        int r = imax(2, size / 8);
        gfx_fill_circle(s, cx, cy - h / 2 - r / 2, r, c);
        gfx_fill_circle(s, cx, cy, r, c);
        gfx_fill_circle(s, cx, cy + h / 2 + r / 2, r, c);
        break;
    }
    case IC_INFO: {
        gfx_circle_outline(s, cx, cy, h, imax(2, size / 10), c);
        gfx_fill_rect(s, cx - imax(1, size / 14), cy - h / 2, imax(2, size / 7), size / 5, c);
        gfx_fill_rect(s, cx - imax(1, size / 14), cy + h / 4, imax(2, size / 7), size / 5, c);
        break;
    }
    case IC_SDCARD: {
        int w = size * 3 / 5, hh = size * 3 / 4;
        int x = cx - w / 2, y = cy - hh / 2;
        gfx_fill_round_rect(s, x, y, w, hh, imax(2, size / 12), c);
        gfx_fill_rect(s, x + w / 5, y, w / 4, hh / 8, 0x000000);
        gfx_fill_rect(s, x + w - w / 5 - w / 4, y, w / 4, hh / 8, 0x000000);
        break;
    }
    case IC_SIGNAL:
        rounded_bars(s, cx, cy, size, c);
        break;
    case IC_BATTERY: {
        int w = size, hh = size / 2;
        int x = cx - w / 2, y = cy - hh / 2;
        gfx_round_rect_outline(s, x, y, w - size / 8, hh, imax(2, size / 10), imax(2, size / 16), c);
        gfx_fill_round_rect(s, x + w - size / 8, cy - hh / 5, size / 10, hh * 2 / 5, 1, c);
        gfx_fill_round_rect(s, x + imax(2, size / 12), y + imax(2, size / 12),
                            (w - size / 8) * 2 / 3, hh - 2 * imax(2, size / 12), 1, c);
        break;
    }
    case IC_HOME: {
        gfx_fill_triangle(s, cx, cy - h, cx - h, cy, cx + h, cy, c);
        gfx_fill_rect(s, cx - h * 3 / 4, cy, h * 3 / 2, h * 3 / 4, c);
        break;
    }
    case IC_IMAGE: {
        int r = imax(2, size / 8);
        gfx_round_rect_outline(s, cx - h, cy - h * 4 / 5, size, size * 4 / 5, imax(2, size / 10), r, c);
        gfx_fill_circle(s, cx - h / 3, cy - h / 3, imax(2, size / 10), c);
        gfx_fill_triangle(s, cx - h, cy + h * 3 / 5, cx - h / 6, cy - h / 5, cx + h / 3, cy + h * 3 / 5, c);
        gfx_fill_triangle(s, cx + h / 6, cy + h * 3 / 5, cx + h / 2, cy, cx + h, cy + h * 3 / 5, c);
        break;
    }
    case IC_MOVIE: {
        gfx_fill_round_rect(s, cx - h, cy - h * 4 / 5, size, size * 4 / 5, imax(2, size / 12), c);
        for (int i = 0; i < 3; i++) {
            gfx_fill_rect(s, cx - h + imax(2, size / 10) + i * size / 3, cy - h * 4 / 5,
                          size / 8, size / 6, 0x000000);
            gfx_fill_rect(s, cx - h + imax(2, size / 10) + i * size / 3, cy + h * 3 / 5 - size / 6,
                          size / 8, size / 6, 0x000000);
        }
        break;
    }
    case IC_SEARCH: {
        int r = size * 2 / 5;
        gfx_circle_outline(s, cx - size / 10, cy - size / 10, r, imax(2, size / 8), c);
        gfx_fill_rect(s, cx + r / 2 - size / 10, cy + r / 2 - size / 10, size / 3, imax(2, size / 9), c);
        break;
    }
    case IC_CLOCK: {
        gfx_circle_outline(s, cx, cy, h, imax(2, size / 10), c);
        gfx_fill_rect(s, cx - imax(1, size / 16), cy - h / 2, imax(2, size / 9), h * 3 / 4, c);
        gfx_fill_rect(s, cx, cy - imax(1, size / 16), h * 3 / 5, imax(2, size / 9), c);
        break;
    }
    case IC_SUN: {
        gfx_fill_circle(s, cx, cy, size / 5, c);
        for (int i = 0; i < 8; i++) {
            static const int dx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
            static const int dy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
            gfx_fill_rect(s, cx + dx[i] * h * 3 / 4 - imax(1, size / 16),
                          cy + dy[i] * h * 3 / 4 - imax(1, size / 16),
                          imax(2, size / 8), imax(2, size / 8), c);
        }
        break;
    }
    case IC_ZOOM: {
        gfx_circle_outline(s, cx, cy, h * 3 / 4, imax(2, size / 9), c);
        gfx_fill_rect(s, cx - h / 4, cy - imax(1, size / 18), h / 2, imax(2, size / 9), c);
        break;
    }
    case IC_LOCK: {
        gfx_fill_round_rect(s, cx - h * 2 / 3, cy - h / 5, size * 2 / 3, size * 3 / 5, imax(2, size / 10), c);
        gfx_circle_outline(s, cx, cy - h / 3, h / 3, imax(2, size / 7), c);
        break;
    }
    case IC_REFRESH: {
        gfx_circle_outline(s, cx, cy, h, imax(2, size / 8), c);
        gfx_fill_triangle(s, cx + h / 2, cy - h - h / 4, cx + h + h / 2, cy - h / 3, cx + h / 2, cy - h / 3, c);
        break;
    }
    default:
        gfx_fill_circle(s, cx, cy, h, c);
        break;
    }
}

void icon_draw_a(Surface *s, IconId id, int cx, int cy, int size, uint32_t color, int alpha)
{
    if (alpha >= 255) { icon_draw(s, id, cx, cy, size, color); return; }
    /* 画到临时画布再整体带透明度贴图（跳过未绘制像素） */
    Surface *tmp = gfx_surface_new_keyed(size + 8, size + 8);
    if (!tmp) { icon_draw(s, id, cx, cy, size, color); return; }
    icon_draw(tmp, id, (size + 8) / 2, (size + 8) / 2, size, color);
    gfx_blit_keyed_a(s, cx - (size + 8) / 2, cy - (size + 8) / 2, tmp, alpha);
    gfx_surface_free(tmp);
}

/* ---------- 图标缓存 ---------- */
#define ICON_CACHE_MAX 64
typedef struct {
    IconId id; int size; uint32_t color; Surface *surf;
} IconCacheEnt;
static IconCacheEnt icon_cache[ICON_CACHE_MAX];
static int icon_cache_n = 0;

void icon_draw_cached(Surface *s, IconId id, int cx, int cy, int size, uint32_t color)
{
    for (int i = 0; i < icon_cache_n; i++) {
        IconCacheEnt *e = &icon_cache[i];
        if (e->id == id && e->size == size && e->color == color) {
            gfx_blit_keyed(s, cx - size / 2 - 4, cy - size / 2 - 4, e->surf);
            return;
        }
    }
    Surface *tmp = gfx_surface_new_keyed(size + 8, size + 8);
    if (!tmp) { icon_draw(s, id, cx, cy, size, color); return; }
    icon_draw(tmp, id, (size + 8) / 2, (size + 8) / 2, size, color);
    if (icon_cache_n < ICON_CACHE_MAX) {
        icon_cache[icon_cache_n].id = id;
        icon_cache[icon_cache_n].size = size;
        icon_cache[icon_cache_n].color = color;
        icon_cache[icon_cache_n].surf = tmp;
        icon_cache_n++;
        gfx_blit_keyed(s, cx - size / 2 - 4, cy - size / 2 - 4, tmp);
        
    } else {
        gfx_blit_keyed(s, cx - size / 2 - 4, cy - size / 2 - 4, tmp);
        gfx_surface_free(tmp);
    }
}

/* ================================================================== */
/* 桌面用的大图标（彩色，带渐变底板）                                    */
/* ================================================================== */
static Surface *appicon_cache[APPICON_COUNT];
static int appicon_size = 0;

static void draw_appicon(Surface *s, AppIconId id, int size)
{
    int S = size;
    uint32_t top, bot;
    switch (id) {
    case APPICON_CAMERA:  top = RGB(0x3d, 0xd6, 0xa0); bot = RGB(0x14, 0x9e, 0x7a); break;
    case APPICON_VIDEO:   top = RGB(0xff, 0x7b, 0x6b); bot = RGB(0xd6, 0x3a, 0x3a); break;
    case APPICON_GALLERY: top = RGB(0x63, 0xa8, 0xff); bot = RGB(0x2f, 0x5f, 0xd0); break;
    case APPICON_CHAT:    top = RGB(0xa9, 0x8b, 0xff); bot = RGB(0x6a, 0x45, 0xd8); break;
    case APPICON_BRICK:   top = RGB(0xff, 0xa9, 0x3d); bot = RGB(0xd6, 0x62, 0x14); break;
    case APPICON_MUSIC:   top = RGB(0xff, 0x7a, 0xb0); bot = RGB(0xc4, 0x2a, 0x74); break;
    default:              top = RGB(0x88, 0x88, 0x88); bot = RGB(0x55, 0x55, 0x55); break;
    }
    /* 底板：圆角 + 竖向渐变 + 顶部高光 */
    for (int y = 0; y < S; y++) {
        uint32_t c = gfx_blend(top, bot, y * 255 / (S - 1));
        gfx_fill_rect(s, 0, y, S, 1, c);
    }
    gfx_fill_rect_a(s, 0, 0, S, S / 3, 0xffffff, 18);
    gfx_fill_round_rect_a(s, 0, 0, S, S, S / 5, 0xffffff, 12);
    /* 最后再把圆角外的部分清成透明键（必须在所有绘制之后，否则抗锯齿会污染透明区） */
    {
        Surface *mask = gfx_surface_new(S, S);
        if (mask) {
            gfx_fill_round_rect(mask, 0, 0, S, S, S / 5, 0xffffff);
            for (int y = 0; y < S; y++) {
                for (int x = 0; x < S; x++) {
                    if (mask->px[(size_t)y * mask->stride + x] != 0xffffff)
                        s->px[(size_t)y * s->stride + x] = GX_KEY;
                }
            }
            gfx_surface_free(mask);
        }
    }

    int c = S / 2;
    uint32_t w = 0xffffff;
    if (id == APPICON_CAMERA) {
        /* 相机：机身 + 顶部取景器凸起 + 镜头（白圈 + 深色镜片 + 高光） */
        int bw = S * 62 / 100, bh = S * 44 / 100;
        int x = c - bw / 2, y = c - bh / 2 + S / 18;
        int rad = S / 10;
        gfx_fill_round_rect(s, c - S / 6, y - S / 12, S / 3, S / 12 + 2, S / 40, w);
        gfx_fill_round_rect(s, x, y, bw, bh, rad, w);
        int lr = bh * 34 / 100;
        gfx_fill_circle(s, c, y + bh / 2, lr, bot);
        gfx_fill_circle(s, c, y + bh / 2, lr - S / 28, RGB(0x1b, 0x2a, 0x2e));
        gfx_fill_circle_a(s, c - lr / 3, y + bh / 2 - lr / 3, lr / 4, w, 220);
        gfx_fill_circle(s, x + bw - S / 9, y + S / 12, S / 40, RGB(0xff, 0xe0, 0x70));
    } else if (id == APPICON_VIDEO) {
        /* 摄像机：机身 + 右侧镜头三角 + 录制红点 */
        int bh = S * 42 / 100, bw = S * 46 / 100;
        int x = c - bw / 2 - S / 10, y = c - bh / 2;
        gfx_fill_round_rect(s, x, y, bw, bh, S / 10, w);
        gfx_fill_triangle(s, x + bw, c - bh / 5, x + bw, c + bh / 5,
                          x + bw + S / 7, c, w);
        gfx_fill_triangle(s, c - S / 22, c - S / 12, c - S / 22, c + S / 12,
                          c + S / 12, c, RGB(0xe5, 0x39, 0x35));
    } else if (id == APPICON_GALLERY) {
        int bw = S * 62 / 100, bh = S * 48 / 100;
        int x = c - bw / 2, y = c - bh / 2 + S / 24;
        gfx_fill_round_rect(s, x + S / 16, y - S / 16, bw, bh, S / 16, w);
        gfx_fill_round_rect(s, x, y, bw, bh, S / 16, RGB(0x22, 0x4d, 0xa8));
        gfx_fill_circle(s, x + bw / 4, y + bh / 4, S / 18, w);
        gfx_fill_triangle(s, x + 2, y + bh - 3, x + bw / 2, y + bh / 3, x + bw * 3 / 4, y + bh - 3, w);
        gfx_fill_triangle(s, x + bw / 3, y + bh - 3, x + bw * 3 / 4, y + bh / 2, x + bw - 2, y + bh - 3, w);
    } else if (id == APPICON_CHAT) {
        int bw = S * 64 / 100, bh = S * 48 / 100;
        int x = c - bw / 2, y = c - bh / 2;
        gfx_fill_round_rect(s, x, y, bw, bh, S / 8, w);
        gfx_fill_triangle(s, x + bw / 5, y + bh - 2, x + bw / 5 + S / 10, y + bh - 2,
                          x + bw / 8, y + bh + S / 8, w);
        for (int i = 0; i < 3; i++)
            gfx_fill_circle(s, x + bw / 4 + i * bw / 4, y + bh / 2, S / 22, top);
        /* 右上角小星星 */
        int sx = x + bw - S / 12, sy = y - S / 20;
        gfx_fill_circle(s, sx, sy, S / 22, RGB(0xff, 0xe0, 0x7a));
    } else if (id == APPICON_BRICK) {
        /* 打砖块：三行小砖 + 小球 + 挡板 */
        int bw = S * 24 / 100, bh = S * 11 / 100, gp = S * 5 / 100;
        int x0 = c - (2 * bw + gp) / 2, y0 = S * 22 / 100;
        static const uint32_t rowcol[3] = { 0xff8a8a, 0xffd77a, 0xffffff };
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 2; k++)
                gfx_fill_round_rect(s, x0 + k * (bw + gp), y0 + r * (bh + gp),
                                    bw, bh, S / 40, rowcol[r]);
        gfx_fill_circle(s, c - S * 22 / 100, y0 + 3 * (bh + gp) + S / 10, S / 16, w);
        gfx_fill_round_rect(s, c - S * 24 / 100, S * 78 / 100, S * 48 / 100,
                            S * 9 / 100, S / 30, RGB(0x2a, 0x1c, 0x0e));
    } else if (id == APPICON_MUSIC) {
        /* 音乐：黑胶唱片 + 标签 + 右上角音符 */
        int r = S * 30 / 100;
        gfx_fill_circle(s, c, c, r, RGB(0x2b, 0x1f, 0x2a));
        gfx_circle_outline_a(s, c, c, r - S / 10, 1, w, 60);
        gfx_circle_outline_a(s, c, c, r - S / 6, 1, w, 40);
        gfx_fill_circle(s, c, c, S * 11 / 100, RGB(0xff, 0xd0, 0xe4));
        gfx_fill_circle(s, c, c, S / 24, top);
        int nx = c + S * 24 / 100, ny = c - S * 22 / 100;
        gfx_fill_circle(s, nx - S / 30, ny + S / 11, S / 17, w);
        gfx_fill_rect(s, nx + S / 60, ny - S / 8, S / 26, S / 9, w);
        gfx_fill_rect(s, nx + S / 60, ny - S / 8, S / 10, S / 40, w);
    }
}

Surface *appicon_get(AppIconId id, int size)
{
    if (id < 0 || id >= APPICON_COUNT) return NULL;
    if (appicon_size != size) {
        for (int i = 0; i < APPICON_COUNT; i++) {
            if (appicon_cache[i]) { gfx_surface_free(appicon_cache[i]); appicon_cache[i] = NULL; }
        }
        appicon_size = size;
    }
    if (!appicon_cache[id]) {
        Surface *s = gfx_surface_new_keyed(size, size);
        if (!s) return NULL;
        draw_appicon(s, id, size);
        appicon_cache[id] = s;
    }
    return appicon_cache[id];
}

void appicon_preload(int size)
{
    for (int i = 0; i < APPICON_COUNT; i++) appicon_get((AppIconId)i, size);
}

/* ================================================================== */
/* 控件                                                                */
/* ================================================================== */
void ui_card(Surface *s, int x, int y, int w, int h, int radius)
{
    gfx_fill_round_rect_a(s, x, y + 3, w, h, radius, C_BLACK, 60);
    gfx_fill_round_rect(s, x, y, w, h, radius, C_SURFACE);
}

void ui_topbar(Surface *s, const char *title, const char *subtitle, int show_back)
{
    int H = UI_TOPBAR_H;
    gfx_fill_rect(s, 0, 0, s->w, H, C_BG);
    gfx_fill_rect(s, 0, H - 1, s->w, 1, C_LINE);
    if (show_back) {
        gfx_fill_circle_a(s, 32, H / 2, 19, C_SURFACE2, 255);
        icon_draw_cached(s, IC_BACK, 32, H / 2, 20, C_TEXT);
    }
    int tx = show_back ? 64 : 20;
    if (subtitle && *subtitle) {
        text_draw_vcenter(s, tx, 0, H / 2 + 6, title, FONT_BODY, C_TEXT);
        text_draw_vcenter(s, tx, H / 2 - 2, H / 2, subtitle, FONT_SMALL, C_TEXT_MUTED);
    } else {
        text_draw_vcenter(s, tx, 0, H, title, FONT_TITLE, C_TEXT);
    }
}

int ui_topbar_back_hit(int x, int y)
{
    return ui_hit(x, y, 8, 8, 48, UI_TOPBAR_H - 16);
}

void ui_button(Surface *s, int x, int y, int w, int h, const char *label,
               uint32_t bg, uint32_t fg, int pressed, int radius)
{
    if (pressed) bg = gfx_blend(bg, 0x000000, 60);
    gfx_fill_round_rect(s, x, y, w, h, radius, bg);
    if (label && *label) text_draw_vcenter_center(s, x + w / 2, y, h, label, FONT_BODY, fg);
}

int ui_pill_width(const char *label)
{
    return text_width(FONT_SMALL, label) + 26;
}

void ui_pill(Surface *s, int x, int y, const char *label, uint32_t bg, uint32_t fg)
{
    int w = ui_pill_width(label), h = 28;
    gfx_fill_round_rect(s, x, y, w, h, h / 2, bg);
    text_draw_vcenter_center(s, x + w / 2, y, h, label, FONT_SMALL, fg);
}

void ui_progress(Surface *s, int x, int y, int w, int h, float p, uint32_t bg, uint32_t fg)
{
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    gfx_fill_round_rect(s, x, y, w, h, h / 2, bg);
    int fw = (int)(w * p);
    if (fw > 2) gfx_fill_round_rect(s, x, y, fw, h, h / 2, fg);
}

void ui_spinner(Surface *s, int cx, int cy, int r, int thick, uint32_t color, float phase)
{
    int seg = 8;
    for (int i = 0; i < seg; i++) {
        float a0 = phase + (float)i / seg * 6.2831853f;
        int a = 40 + 200 * i / seg;
        int x0 = cx + (int)(__builtin_cosf(a0) * (r - thick / 2));
        int y0 = cy + (int)(__builtin_sinf(a0) * (r - thick / 2));
        gfx_fill_circle_a(s, x0, y0, thick / 2, color, a);
    }
}

void ui_dimmer(Surface *s, int alpha)
{
    gfx_fill_rect_a(s, 0, 0, s->w, s->h, C_BLACK, alpha);
}

/* ---------- 状态栏 ---------- */
int ui_status_bar_h(void) { return STATUS_BAR_H; }

void ui_status_bar(Surface *s, const char *right_text)
{
    gfx_fill_rect(s, 0, 0, s->w, STATUS_BAR_H, C_BG);
    char t[16];
    ui_time_str(t, sizeof(t));
    text_draw_vcenter(s, 16, 0, STATUS_BAR_H, t, FONT_SMALL, C_TEXT);
    /* 右侧：可选文本 + 信号 + 电量 */
    int rx = s->w - 16;
    icon_draw_cached(s, IC_BATTERY, rx - 14, STATUS_BAR_H / 2, 26, C_TEXT);
    rx -= 34;
    icon_draw_cached(s, IC_SIGNAL, rx - 10, STATUS_BAR_H / 2, 20, C_TEXT);
    rx -= 26;
    if (right_text && *right_text) {
        int w = text_width(FONT_SMALL, right_text);
        text_draw_vcenter(s, rx - w, 0, STATUS_BAR_H, right_text, FONT_SMALL, C_TEXT_DIM);
    }
}

/* ---------- 对话框 ---------- */
#define DLG_W 460
#define DLG_H 220
#define DLG_X ((SCREEN_W - DLG_W) / 2)
#define DLG_Y ((SCREEN_H - DLG_H) / 2)
#define DLG_BTN_W 150
#define DLG_BTN_H 52

void ui_dialog(Surface *s, const char *title, const char *msg,
               const char *ok_label, const char *cancel_label)
{
    ui_dimmer(s, 150);
    ui_card(s, DLG_X, DLG_Y, DLG_W, DLG_H, 18);
    text_draw_vcenter_center(s, DLG_X + DLG_W / 2, DLG_Y + 22, 34, title, FONT_BODY, C_TEXT);
    if (msg && *msg)
        text_draw_vcenter_center(s, DLG_X + DLG_W / 2, DLG_Y + 66, 30, msg, FONT_SMALL, C_TEXT_DIM);
    int by = DLG_Y + DLG_H - DLG_BTN_H - 24;
    if (cancel_label && *cancel_label) {
        ui_button(s, DLG_X + 36, by, DLG_BTN_W, DLG_BTN_H, cancel_label, C_SURFACE2, C_TEXT, 0, 12);
        ui_button(s, DLG_X + DLG_W - 36 - DLG_BTN_W, by, DLG_BTN_W, DLG_BTN_H, ok_label, C_DANGER, C_WHITE, 0, 12);
    } else {
        ui_button(s, DLG_X + (DLG_W - DLG_BTN_W) / 2, by, DLG_BTN_W, DLG_BTN_H, ok_label, C_ACCENT, C_BLACK, 0, 12);
    }
}

int ui_dialog_ok_hit(int x, int y)
{
    int by = DLG_Y + DLG_H - DLG_BTN_H - 24;
    return ui_hit(x, y, DLG_X + DLG_W - 36 - DLG_BTN_W, by, DLG_BTN_W, DLG_BTN_H);
}

int ui_dialog_cancel_hit(int x, int y)
{
    int by = DLG_Y + DLG_H - DLG_BTN_H - 24;
    return ui_hit(x, y, DLG_X + 36, by, DLG_BTN_W, DLG_BTN_H);
}

/* ---------- Toast ---------- */
static int toast_bottom = 96;      /* 距屏幕底部的距离，应用可调整 */
void ui_toast_set_bottom(int px) { toast_bottom = px; }

static char toast_msg[128] = {0};
static uint64_t toast_t0 = 0;
static int toast_on = 0;

void ui_toast(const char *msg)
{
    if (!msg) return;
    snprintf(toast_msg, sizeof(toast_msg), "%s", msg);
    toast_t0 = now_ms();
    toast_on = 1;
}

int ui_toast_active(void)
{
    if (!toast_on) return 0;
    uint64_t d = now_ms() - toast_t0;
    if (d > 2400) { toast_on = 0; return 0; }
    return 1;
}

void ui_toast_draw(Surface *s, uint64_t t_ms)
{
    if (!toast_on) return;
    (void)t_ms;
    uint64_t d = now_ms() - toast_t0;
    if (d > 2400) { toast_on = 0; return; }
    int a = 255;
    if (d < 180) a = (int)(d * 255 / 180);
    else if (d > 2000) a = (int)((2400 - d) * 255 / 400);
    a = iclamp(a, 0, 255);

    int tw = text_width(FONT_BODY, toast_msg);
    int w = tw + 56, h = 46;
    int x = (s->w - w) / 2, y = s->h - toast_bottom;
    gfx_fill_round_rect_a(s, x, y + 3, w, h, h / 2, C_BLACK, 90 * a / 255);
    gfx_fill_round_rect_a(s, x, y, w, h, h / 2, RGB(0x2a, 0x33, 0x40), 245 * a / 255);
    text_draw_vcenter_center(s, s->w / 2, y, h, toast_msg, FONT_BODY,
                             gfx_blend(RGB(0x2a, 0x33, 0x40), C_TEXT, a));
}

/* ---------- 时间文案 ---------- */
void ui_time_str(char *buf, int n)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(buf, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

void ui_date_str(char *buf, int n)
{
    static const char *wd[7] = { "日", "一", "二", "三", "四", "五", "六" };
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(buf, n, "%d月%d日 星期%s", lt.tm_mon + 1, lt.tm_mday, wd[lt.tm_wday % 7]);
}

void ui_datetime_str(char *buf, int n)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec);
}
