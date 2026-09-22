/*
 * app_gallery.c — 图库：网格浏览 / 大图查看 / 视频播放 / 删除
 *
 * 交互：
 *   网格页   左右滑动或拖动翻页，轻触缩略图进入查看，顶部标签切换 全部/照片/视频
 *   查看页   左右滑动切换上一张/下一张，轻触隐藏/显示工具条，
 *            右上角垃圾桶删除（带二次确认）
 *   视频     查看页中央播放/暂停，底部进度条与时间
 */
#include "apps.h"
#include "camera.h"
#include "image.h"
#include "media.h"
#include "avi.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 布局 */
#define TB_H        58
#define TAB_Y       66
#define TAB_H       34
#define GRID_Y      108
#define COLS        4
#define ROWS        2
#define PER_PAGE    (COLS * ROWS)
#define CELL_W      186
#define CELL_H      144
#define CELL_GAP    8
#define GRID_X      ((SCREEN_W - (COLS * CELL_W + (COLS - 1) * CELL_GAP)) / 2)
#define DOTS_Y      408
#define VIEW_TOP    TB_H

/* 状态 */
typedef enum { GAL_GRID = 0, GAL_VIEW } GalMode;

static MediaItem items[MEDIA_MAX_ITEMS];
static int  nitems = 0;
static int  flist[MEDIA_MAX_ITEMS];
static int  fn = 0;
static int  filter = -1;              /* -1 全部 / MEDIA_PHOTO / MEDIA_VIDEO */
static int  page = 0;
static GalMode mode = GAL_GRID;
static int  view_idx = -1;            /* flist 下标 */
static int  del_confirm = 0;
static int  immersive = 0;            /* 查看页隐藏工具条 */

/* 拖动 / 翻页动画 */
static int  dragging = 0;
static int  drag_x0 = 0, drag_dx = 0;
static float page_slide = 0;
static uint64_t page_anim_t0 = 0;
static int  page_anim_from = 0;

/* 查看页资源 */
static Surface *view_img = NULL;
static char     view_path[320] = {0};
static int      view_loaded = -1;

/* 视频播放 */
static AviReader *vr = NULL;
static char     vr_path[320] = {0};
static Surface *vr_frame = NULL;
static int      vr_playing = 0;
static float    vr_pos = 0;           /* 当前帧位置（浮点，按时间推进） */
static uint64_t vr_t0 = 0;

static void rebuild_list(void)
{
    fn = 0;
    for (int i = 0; i < nitems; i++) {
        if (filter < 0 || items[i].type == filter) flist[fn++] = i;
    }
    int pages = (fn + PER_PAGE - 1) / PER_PAGE;
    if (pages < 1) pages = 1;
    if (page > pages - 1) page = pages - 1;
    if (page < 0) page = 0;
}

static void reload(void)
{
    nitems = media_scan(items, MEDIA_MAX_ITEMS);
    rebuild_list();
}

static void view_unload(void)
{
    if (view_img) { gfx_surface_free(view_img); view_img = NULL; }
    view_loaded = -1;
    view_path[0] = 0;
}

static void video_close(void)
{
    if (vr) { avi_close(vr); vr = NULL; }
    if (vr_frame) { gfx_surface_free(vr_frame); vr_frame = NULL; }
    vr_path[0] = 0;
    vr_playing = 0;
    vr_pos = 0;
}

void app_gallery_enter(void)
{
    ui_toast_set_bottom(96);
    reload();
    mode = GAL_GRID;
    del_confirm = 0;
    immersive = 0;
    dragging = 0;
    drag_dx = 0;
    page_slide = 0;
    view_unload();
    video_close();
}

void app_gallery_leave(void)
{
    view_unload();
    video_close();
    media_thumb_cache_clear();
}

/* 供其它应用直接打开某张（media_index 为 items 下标，-1 表示只进网格） */
void app_gallery_open_view(int media_index)
{
    reload();
    if (media_index >= 0) {
        for (int i = 0; i < fn; i++) {
            if (flist[i] == media_index) {
                view_idx = i;
                mode = GAL_VIEW;
                return;
            }
        }
    }
    mode = GAL_GRID;
}

/* ================= 网格页 ================= */
static void draw_tabs(Surface *s)
{
    static const char *labels[3] = { "全部", "照片", "视频" };
    static const int vals[3] = { -1, MEDIA_PHOTO, MEDIA_VIDEO };
    int total = 0;
    int widths[3];
    for (int i = 0; i < 3; i++) { widths[i] = text_width(FONT_BODY, labels[i]) + 34; total += widths[i]; }
    total += 2 * 8;
    int x = (SCREEN_W - total) / 2;
    for (int i = 0; i < 3; i++) {
        int active = (filter == vals[i]);
        gfx_fill_round_rect(s, x, TAB_Y, widths[i], TAB_H, TAB_H / 2,
                            active ? C_ACCENT : C_SURFACE2);
        text_draw_vcenter_center(s, x + widths[i] / 2, TAB_Y, TAB_H, labels[i], FONT_BODY,
                                 active ? C_BLACK : C_TEXT_DIM);
        x += widths[i] + 8;
    }
}

static void draw_grid_page(Surface *s, int p, int offset_x)
{
    int start = p * PER_PAGE;
    for (int k = 0; k < PER_PAGE; k++) {
        int fi = start + k;
        int col = k % COLS, row = k / COLS;
        int x = GRID_X + col * (CELL_W + CELL_GAP) + offset_x;
        int y = GRID_Y + row * (CELL_H + CELL_GAP);
        /* 视口外跳过 */
        if (x + CELL_W < 0 || x > SCREEN_W) continue;

        if (fi >= fn) {
            /* 空格子：只画淡淡底 */
            gfx_fill_round_rect_a(s, x, y, CELL_W, CELL_H, 10, C_SURFACE, 70);
            continue;
        }
        MediaItem *it = &items[flist[fi]];
        gfx_set_clip(0, 0, SCREEN_W, SCREEN_H);
        Surface *th = media_thumb(it);
        if (th) {
            gfx_blit_scaled(s, x, y, CELL_W, CELL_H, th, 0, 0, th->w, th->h, 255);
        } else {
            gfx_fill_round_rect(s, x, y, CELL_W, CELL_H, 10, C_SURFACE2);
            icon_draw_cached(s, IC_IMAGE, x + CELL_W / 2, y + CELL_H / 2, 34, C_TEXT_MUTED);
        }
        /* 圆角遮罩：四角涂回背景色 */
        for (int j = 0; j < 12; j++) {
            int cut = 12 - (int)__builtin_sqrtf((float)(144 - (12 - j) * (12 - j)));
            gfx_fill_rect(s, x, y + j, cut, 1, C_BG);
            gfx_fill_rect(s, x + CELL_W - cut, y + j, cut, 1, C_BG);
            gfx_fill_rect(s, x, y + CELL_H - 1 - j, cut, 1, C_BG);
            gfx_fill_rect(s, x + CELL_W - cut, y + CELL_H - 1 - j, cut, 1, C_BG);
        }
        gfx_round_rect_outline(s, x, y, CELL_W, CELL_H, 10, 1, RGB(0x33, 0x3d, 0x4a));

        /* 视频角标 */
        if (it->type == MEDIA_VIDEO) {
            int bw = 62, bh = 24;
            gfx_fill_round_rect_a(s, x + 8, y + CELL_H - bh - 8, bw, bh, 6, RGB(0, 0, 0), 150);
            icon_draw_cached(s, IC_PLAY, x + 20, y + CELL_H - bh / 2 - 8, 12, C_WHITE);
            char db[16];
            snprintf(db, sizeof(db), "%d:%02d", it->duration_s / 60, it->duration_s % 60);
            text_draw_vcenter(s, x + 30, y + CELL_H - bh - 8, bh, db, FONT_SMALL, C_WHITE);
        }
    }
}

static void draw_grid(Surface *s)
{
    gfx_fill_rect(s, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    ui_status_bar(s, NULL);

    char sub[128];
    int np = media_count_type(items, nitems, MEDIA_PHOTO);
    int nv = media_count_type(items, nitems, MEDIA_VIDEO);
    snprintf(sub, sizeof(sub), "%d 张照片 · %d 个视频", np, nv);
    ui_topbar(s, "图库", sub, 1);
    draw_tabs(s);

    int pages = (fn + PER_PAGE - 1) / PER_PAGE;
    if (pages < 1) pages = 1;
    int off = (int)page_slide;
    draw_grid_page(s, page, off);
    if (off > 0 && page > 0)     draw_grid_page(s, page - 1, off - SCREEN_W);
    if (off < 0 && page < pages - 1) draw_grid_page(s, page + 1, off + SCREEN_W);

    /* 空状态 */
    if (fn == 0) {
        icon_draw_cached(s, IC_IMAGE, SCREEN_W / 2, 220, 64, RGB(0x3a, 0x44, 0x52));
        text_draw_center(s, SCREEN_W / 2, 268, filter == MEDIA_VIDEO ? "还没有录像" : "还没有照片",
                         FONT_TITLE, C_TEXT_DIM);
        text_draw_center(s, SCREEN_W / 2, 306, "回到桌面打开「拍照」或「录像」试试",
                         FONT_SMALL, C_TEXT_MUTED);
    }

    /* 页码点 */
    if (pages > 1) {
        int total = pages * 18;
        int x0 = (SCREEN_W - total) / 2 + 9;
        for (int i = 0; i < pages && i < 12; i++) {
            gfx_fill_circle_a(s, x0 + i * 18, DOTS_Y, i == page ? 5 : 3,
                              i == page ? C_TEXT : C_TEXT_MUTED, i == page ? 240 : 140);
        }
    }
    /* 底部提示 */
    const char *hint = fn > 0 ? "轻触查看 · 左右滑动翻页" : "";
    int hw = text_width(FONT_SMALL, hint);
    text_draw_vcenter(s, (SCREEN_W - hw) / 2, SCREEN_H - 46, 30, hint, FONT_SMALL, C_TEXT_MUTED);
}

/* ================= 查看页 ================= */
static void view_load_current(void)
{
    if (view_idx < 0 || view_idx >= fn) return;
    MediaItem *it = &items[flist[view_idx]];
    if (it->type == MEDIA_VIDEO) {
        if (!strcmp(vr_path, it->path)) return;
        video_close();
        vr = avi_open(it->path);
        if (vr) {
            snprintf(vr_path, sizeof(vr_path), "%s", it->path);
            vr_frame = gfx_surface_new(avi_width(vr) > 0 ? avi_width(vr) : CAM_FRAME_W,
                                       avi_height(vr) > 0 ? avi_height(vr) : CAM_FRAME_H);
            vr_pos = 0;
            vr_playing = 1;
            vr_t0 = now_ms();
        }
        return;
    }
    if (!strcmp(view_path, it->path) && view_img) return;
    video_close();
    view_unload();
    view_img = image_load_file(it->path);
    snprintf(view_path, sizeof(view_path), "%s", it->path);
    view_loaded = view_img ? 0 : -1;
}

static void video_seek(Surface *s, int frame)
{
    if (!vr || !vr_frame) return;
    if (frame < 0) frame = 0;
    int fc = avi_frame_count(vr);
    if (frame >= fc) frame = fc - 1;
    int len = 0;
    const unsigned char *jpeg = avi_frame(vr, frame, &len);
    if (!jpeg) return;
    Surface *tmp = image_load_mem(jpeg, len);
    if (!tmp) return;
    /* 缩放到视频帧缓冲尺寸 */
    if (tmp->w == vr_frame->w && tmp->h == vr_frame->h) {
        for (int y = 0; y < tmp->h; y++)
            memcpy(vr_frame->px + (size_t)y * vr_frame->stride,
                   tmp->px + (size_t)y * tmp->stride, (size_t)tmp->w * 4);
    } else {
        Surface *sc = image_scale(tmp, vr_frame->w, vr_frame->h);
        if (sc) {
            for (int y = 0; y < sc->h; y++)
                memcpy(vr_frame->px + (size_t)y * vr_frame->stride,
                       sc->px + (size_t)y * sc->stride, (size_t)sc->w * 4);
            gfx_surface_free(sc);
        }
    }
    gfx_surface_free(tmp);
    (void)s;
}

static void video_update(void)
{
    if (!vr || !vr_playing) return;
    int fps = avi_fps(vr);
    if (fps <= 0) fps = 15;
    uint64_t now = now_ms();
    float dt = (float)(now - vr_t0) / 1000.0f;
    vr_t0 = now;
    vr_pos += dt * fps;
    int fc = avi_frame_count(vr);
    if (vr_pos >= fc) { vr_pos = (float)(fc > 0 ? fc - 1 : 0); vr_playing = 0; }
    int target = (int)vr_pos;
    static int last_decoded = -1;
    if (target != last_decoded) {
        video_seek(NULL, target);
        last_decoded = target;
    }
    if (!vr_playing && target == last_decoded) last_decoded = -1;
}

static void draw_viewer(Surface *s)
{
    MediaItem *it = (view_idx >= 0 && view_idx < fn) ? &items[flist[view_idx]] : NULL;
    gfx_fill_rect(s, 0, 0, SCREEN_W, SCREEN_H, RGB(0x00, 0x00, 0x00));

    if (!it) {
        text_draw_center(s, SCREEN_W / 2, SCREEN_H / 2, "没有可显示的内容", FONT_TITLE, C_TEXT_DIM);
        return;
    }

    if (it->type == MEDIA_VIDEO) {
        video_update();
        if (vr_frame) {
            int avail_h = SCREEN_H;
            int w = vr_frame->w, h = vr_frame->h;
            double sc = (double)avail_h / h;
            if (w * sc > SCREEN_W) sc = (double)SCREEN_W / w;
            int dw = (int)(w * sc), dh = (int)(h * sc);
            gfx_blit_scaled(s, (SCREEN_W - dw) / 2, (SCREEN_H - dh) / 2, dw, dh,
                            vr_frame, 0, 0, w, h, 255);
        } else {
            text_draw_center(s, SCREEN_W / 2, SCREEN_H / 2, "无法播放该视频", FONT_TITLE, C_TEXT_DIM);
        }
    } else {
        if (view_loaded != 0) view_load_current();
        if (view_img) {
            int w = view_img->w, h = view_img->h;
            double sc = 1.0;
            double s1 = (double)SCREEN_W / w, s2 = (double)SCREEN_H / h;
            sc = s1 < s2 ? s1 : s2;
            int dw = (int)(w * sc), dh = (int)(h * sc);
            gfx_blit_scaled(s, (SCREEN_W - dw) / 2, (SCREEN_H - dh) / 2, dw, dh,
                            view_img, 0, 0, w, h, 255);
        } else {
            icon_draw_cached(s, IC_IMAGE, SCREEN_W / 2, SCREEN_H / 2 - 20, 60, C_TEXT_MUTED);
            text_draw_center(s, SCREEN_W / 2, SCREEN_H / 2 + 40, "图片已损坏或无法读取", FONT_BODY, C_TEXT_DIM);
        }
    }

    /* 左右切换提示箭头 */
    if (view_idx > 0) {
        gfx_fill_circle_a(s, 26, SCREEN_H / 2, 20, RGB(0, 0, 0), 90);
        icon_draw_cached(s, IC_CHEVRON_L, 26, SCREEN_H / 2, 22, RGB(0xff, 0xff, 0xff));
    }
    if (view_idx < fn - 1) {
        gfx_fill_circle_a(s, SCREEN_W - 26, SCREEN_H / 2, 20, RGB(0, 0, 0), 90);
        icon_draw_cached(s, IC_CHEVRON_R, SCREEN_W - 26, SCREEN_H / 2, 22, RGB(0xff, 0xff, 0xff));
    }

    if (!immersive) {
        /* 顶部工具条 */
        for (int i = 0; i < 76; i++)
            gfx_fill_rect_a(s, 0, i, SCREEN_W, 1, RGB(0, 0, 0), 170 - i * 170 / 76);
        gfx_fill_circle_a(s, 32, 32, 20, RGB(0, 0, 0), 80);
        icon_draw_cached(s, IC_BACK, 32, 32, 22, C_WHITE);
        text_draw_vcenter(s, 62, 0, 64, it->name, FONT_BODY, C_WHITE);
        char idx[32];
        snprintf(idx, sizeof(idx), "%d/%d", view_idx + 1, fn);
        text_draw_vcenter(s, 62, 26, 44, idx, FONT_SMALL, C_TEXT_DIM);

        /* 删除按钮 */
        gfx_fill_circle_a(s, SCREEN_W - 34, 32, 22, RGB(0xff, 0x5a, 0x5f), 210);
        icon_draw_cached(s, IC_TRASH, SCREEN_W - 34, 32, 22, C_WHITE);

        /* 底部信息 / 视频控制 */
        int by = SCREEN_H - 84;
        for (int i = 0; i < 84; i++)
            gfx_fill_rect_a(s, 0, by + i, SCREEN_W, 1, RGB(0, 0, 0), i * 170 / 84);
        if (it->type == MEDIA_VIDEO) {
            /* 播放/暂停 */
            gfx_fill_circle_a(s, 42, SCREEN_H - 42, 26, RGB(0xff, 0xff, 0xff), 235);
            icon_draw_cached(s, vr_playing ? IC_PAUSE : IC_PLAY, 42, SCREEN_H - 42, 22, RGB(0, 0, 0));
            int fc = vr ? avi_frame_count(vr) : 0;
            int fps = vr ? avi_fps(vr) : 15;
            if (fps <= 0) fps = 15;
            int cur = (int)vr_pos;
            int cur_s = cur / fps, tot_s = fc / fps;
            char tb[48];
            snprintf(tb, sizeof(tb), "%d:%02d / %d:%02d",
                     cur_s / 60, cur_s % 60, tot_s / 60, tot_s % 60);
            int x0 = 82;
            int barw = SCREEN_W - x0 - 30;
            ui_progress(s, x0, SCREEN_H - 48, barw, 8, fc > 1 ? (float)cur / (fc - 1) : 0,
                        RGB(0xff, 0xff, 0xff), C_ACCENT);
            gfx_fill_circle_a(s, x0 + (int)(barw * (fc > 1 ? (float)cur / (fc - 1) : 0)), SCREEN_H - 44,
                              9, C_WHITE, 255);
            text_draw_right(s, SCREEN_W - 24, SCREEN_H - 30, tb, FONT_SMALL, C_TEXT_DIM);
            char meta[64];
            snprintf(meta, sizeof(meta), "%d 帧 · %d fps · %d KB", fc, vr ? avi_fps(vr) : 0, it->size_kb);
            text_draw(s, x0, SCREEN_H - 24, meta, FONT_SMALL, C_TEXT_MUTED);
        } else {
            char meta[96];
            snprintf(meta, sizeof(meta), "%d KB · %d×%d", it->size_kb,
                     view_img ? view_img->w : 0, view_img ? view_img->h : 0);
            text_draw_vcenter(s, 24, SCREEN_H - 68, 44, meta, FONT_SMALL, C_TEXT_DIM);
            const char *hint = "左右滑动切换 · 轻触隐藏工具条";
            int hw = text_width(FONT_SMALL, hint);
            text_draw_vcenter(s, SCREEN_W - hw - 24, SCREEN_H - 68, 44, hint, FONT_SMALL, C_TEXT_MUTED);
        }
    }
}

/* ================= 事件 ================= */
static void goto_page(int new_page, int from_right)
{
    int pages = (fn + PER_PAGE - 1) / PER_PAGE;
    if (pages < 1) pages = 1;
    if (new_page < 0) new_page = 0;
    if (new_page > pages - 1) new_page = pages - 1;
    if (new_page == page) return;
    page = new_page;
    page_anim_from = from_right ? SCREEN_W : -SCREEN_W;
    page_slide = (float)page_anim_from;
    page_anim_t0 = now_ms();
}

static void view_step(int dir)
{
    int n = view_idx + dir;
    if (n < 0 || n >= fn) return;
    view_idx = n;
    immersive = 0;
    MediaItem *it = &items[flist[view_idx]];
    if (it->type == MEDIA_VIDEO && strcmp(vr_path, it->path) != 0) {
        video_close();
        view_load_current();
    } else if (it->type != MEDIA_VIDEO) {
        video_close();
        view_unload();
        view_load_current();
    }
}

static void do_delete(void)
{
    if (view_idx < 0 || view_idx >= fn) return;
    MediaItem *it = &items[flist[view_idx]];
    char name[64];
    snprintf(name, sizeof(name), "%s", it->name);
    if (media_delete(it) != 0) {
        ui_toast("删除失败");
        del_confirm = 0;
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "已删除 %s", name);
    ui_toast(msg);
    del_confirm = 0;
    video_close();
    view_unload();
    reload();
    if (fn == 0) {
        mode = GAL_GRID;
        view_idx = -1;
    } else {
        if (view_idx >= fn) view_idx = fn - 1;
        view_load_current();
    }
}

static int grid_event(const UiEvent *e)
{
    if (e->type == UI_EV_DOWN) {
        dragging = 1;
        drag_x0 = e->x;
        drag_dx = 0;
        return 0;
    }
    if (e->type == UI_EV_MOVE) {
        if (dragging) {
            drag_dx = e->x - drag_x0;
            int pages = (fn + PER_PAGE - 1) / PER_PAGE;
            if (pages < 1) pages = 1;
            /* 到边界时加阻尼 */
            if ((page == 0 && drag_dx > 0) || (page >= pages - 1 && drag_dx < 0))
                page_slide = drag_dx * 0.35f;
            else
                page_slide = (float)drag_dx;
        }
        return 0;
    }
    if (e->type == UI_EV_UP) {
        int was_drag = dragging;
        dragging = 0;
        if (e->swipe == SWIPE_LEFT) { goto_page(page + 1, 1); drag_dx = 0; return 0; }
        if (e->swipe == SWIPE_RIGHT) { goto_page(page - 1, 0); drag_dx = 0; return 0; }
        if (was_drag && !e->tap) {
            /* 拖动幅度够大也算翻页 */
            if (drag_dx < -60) goto_page(page + 1, 1);
            else if (drag_dx > 60) goto_page(page - 1, 0);
            else page_slide = 0;
            drag_dx = 0;
            return 0;
        }
        page_slide = 0;
        drag_dx = 0;
        if (!e->tap) return 0;

        /* 返回 */
        if (ui_topbar_back_hit(e->x, e->y)) return 1;

        /* 标签 */
        static const int vals[3] = { -1, MEDIA_PHOTO, MEDIA_VIDEO };
        int total = 0, widths[3];
        for (int i = 0; i < 3; i++) { widths[i] = text_width(FONT_BODY, (i == 0) ? "全部" : (i == 1) ? "照片" : "视频") + 34; total += widths[i]; }
        total += 16;
        int x = (SCREEN_W - total) / 2;
        for (int i = 0; i < 3; i++) {
            if (ui_hit(e->x, e->y, x, TAB_Y, widths[i], TAB_H)) {
                if (filter != vals[i]) {
                    filter = vals[i];
                    page = 0;
                    page_slide = 0;
                    rebuild_list();
                }
                return 0;
            }
            x += widths[i] + 8;
        }

        /* 缩略图 */
        if (e->y >= GRID_Y && e->y < GRID_Y + ROWS * (CELL_H + CELL_GAP)) {
            int col = (e->x - GRID_X) / (CELL_W + CELL_GAP);
            int row = (e->y - GRID_Y) / (CELL_H + CELL_GAP);
            int inx = (e->x - GRID_X) % (CELL_W + CELL_GAP);
            int iny = (e->y - GRID_Y) % (CELL_H + CELL_GAP);
            if (col >= 0 && col < COLS && row >= 0 && row < ROWS && inx < CELL_W && iny < CELL_H) {
                int fi = page * PER_PAGE + row * COLS + col;
                if (fi < fn) {
                    view_idx = fi;
                    mode = GAL_VIEW;
                    immersive = 0;
                    view_unload();
                    video_close();
                    view_load_current();
                }
            }
        }
        return 0;
    }
    return 0;
}

static int view_event(const UiEvent *e)
{
    MediaItem *it = (view_idx >= 0 && view_idx < fn) ? &items[flist[view_idx]] : NULL;

    if (del_confirm) {
        if (e->type == UI_EV_UP && e->tap) {
            if (ui_dialog_ok_hit(e->x, e->y)) { do_delete(); return 0; }
            if (ui_dialog_cancel_hit(e->x, e->y)) { del_confirm = 0; return 0; }
            if (!ui_hit(e->x, e->y, 170, 130, 460, 220)) del_confirm = 0;
        }
        return 0;
    }

    if (e->type == UI_EV_UP) {
        if (e->swipe == SWIPE_LEFT) {
            if (it && it->type == MEDIA_VIDEO) {
                /* 视频：滑动跳转进度 */
                if (vr) { vr_pos += avi_frame_count(vr) * 0.1f; }
            } else view_step(1);
            return 0;
        }
        if (e->swipe == SWIPE_RIGHT) {
            if (it && it->type == MEDIA_VIDEO) {
                if (vr) { vr_pos -= avi_frame_count(vr) * 0.1f; if (vr_pos < 0) vr_pos = 0; }
            } else view_step(-1);
            return 0;
        }
        if (!e->tap) return 0;

        /* 工具条按钮 */
        if (!immersive) {
            if (ui_topbar_back_hit(e->x, e->y)) { mode = GAL_GRID; video_close(); view_unload(); page_slide = 0; return 0; }
            if (ui_hit(e->x, e->y, SCREEN_W - 58, 8, 48, 48)) { del_confirm = 1; return 0; }
        }
        /* 视频播放控制 */
        if (it && it->type == MEDIA_VIDEO) {
            if (ui_hit(e->x, e->y, 16, SCREEN_H - 68, 52, 52)) {
                vr_playing = !vr_playing;
                vr_t0 = now_ms();
                immersive = 0;
                return 0;
            }
            /* 进度条点击 */
            int x0 = 82, barw = SCREEN_W - x0 - 30;
            if (vr && ui_hit(e->x, e->y, x0 - 10, SCREEN_H - 60, barw + 20, 34)) {
                float p = fclampf((float)(e->x - x0) / barw, 0.0f, 1.0f);
                vr_pos = p * (avi_frame_count(vr) - 1);
                vr_t0 = now_ms();
                immersive = 0;
                return 0;
            }
        }
        /* 左右箭头 */
        if (view_idx > 0 && ui_hit(e->x, e->y, 0, SCREEN_H / 2 - 30, 52, 60)) { view_step(-1); return 0; }
        if (view_idx < fn - 1 && ui_hit(e->x, e->y, SCREEN_W - 52, SCREEN_H / 2 - 30, 52, 60)) { view_step(1); return 0; }
        /* 轻触切换工具条（点击图片中间区域） */
        if (e->y > 70 && e->y < SCREEN_H - 90) {
            immersive = !immersive;
            return 0;
        }
        return 0;
    }
    return 0;
}

void app_gallery_frame(uint64_t t_ms)
{
    Surface *s = gfx_back();
    if (!s) return;
    gfx_reset_clip();

    /* 翻页动画推进 */
    if (page_slide != 0 && !dragging) {
        uint64_t dt = now_ms() - page_anim_t0;
        float k = fclampf((float)dt / 220.0f, 0.0f, 1.0f);
        page_slide = page_anim_from * (1.0f - ease_out_cubic(k));
        if (k >= 1.0f) page_slide = 0;
    }

    if (mode == GAL_GRID) draw_grid(s);
    else draw_viewer(s);

    if (del_confirm) {
        MediaItem *it = (view_idx >= 0 && view_idx < fn) ? &items[flist[view_idx]] : NULL;
        char msg[160];
        msg[0] = 0;
        if (it) snprintf(msg, sizeof(msg), "%s 将被永久删除", it->name);
        ui_dialog(s, "确定要删除吗？", msg, "删除", "取消");
    }
    (void)t_ms;
}

int app_gallery_event(const UiEvent *e)
{
    if (mode == GAL_GRID) return grid_event(e);
    return view_event(e);
}

const AppDef g_app_gallery = {
    .title = "图库",
    .en = "Gallery",
    .icon = APPICON_GALLERY,
    .needs_camera = 0,
    .enter = app_gallery_enter,
    .leave = app_gallery_leave,
    .frame = app_gallery_frame,
    .event = app_gallery_event
};
