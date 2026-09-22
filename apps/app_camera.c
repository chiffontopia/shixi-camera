/*
 * app_camera.c — 拍照应用
 *
 * 全屏取景 + 手机式快门条：左边是最近一张照片（点开进图库），中间大快门，
 * 右边显示存储余量与相机状态。按下快门有白色闪屏动画与轻提示。
 */
#include "apps.h"
#include "camera.h"
#include "image.h"
#include "media.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 布局 */
#define BAR_H        116
#define SHUTTER_R    40
#define SHUTTER_CX   (SCREEN_W / 2)
#define SHUTTER_CY   (SCREEN_H - BAR_H / 2 - 6)
#define THUMB_SIZE   62
#define THUMB_X      24
#define THUMB_Y      (SCREEN_H - BAR_H / 2 - 6 - THUMB_SIZE / 2)

/* 状态 */
static uint64_t last_seq = 0;
static int   flash_until = 0;       /* 闪屏动画结束时间 */
static uint64_t flash_t0 = 0;
static int   focus_until = 0;
static uint64_t focus_t0 = 0;
static int   focus_x = 0, focus_y = 0;
static int   shutter_pressed = 0;
static char  last_photo[320] = {0};
static Surface *last_thumb = NULL;
static int   saving = 0;
static uint64_t stat_t0 = 0;
static int   shot_count = 0;

static void refresh_last_thumb(void)
{
    if (last_thumb) { gfx_surface_free(last_thumb); last_thumb = NULL; }
    if (!last_photo[0]) return;
    Surface *full = image_load_file(last_photo);
    if (!full) return;
    last_thumb = image_thumbnail(full, 120, 120);
    gfx_surface_free(full);
}

void app_camera_enter(void)
{
    ui_toast_set_bottom(150);
    if (camera_source() != CAM_SRC_V4L2) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            ui_toast("未检测到摄像头：当前是演示画面（可插 USB 摄像头）");
        }
    }
    last_seq = 0;
    flash_until = 0;
    focus_until = 0;
    if (!last_photo[0]) {
        /* 还没拍过：用相册里最新的一张照片当缩略图 */
        static MediaItem items[1];
        if (media_scan(items, 1) > 0 && items[0].type == MEDIA_PHOTO)
            snprintf(last_photo, sizeof(last_photo), "%s", items[0].path);
    }
    if (last_photo[0]) refresh_last_thumb();
}

void app_camera_leave(void)
{
    if (last_thumb) { gfx_surface_free(last_thumb); last_thumb = NULL; }
}

/* ---------------- 拍照 ---------------- */
static void do_capture(void)
{
    if (media_free_mb() < 8) {
        ui_toast("存储空间不足，请先删除部分文件");
        return;
    }
    static unsigned char *jbuf = NULL;
    if (!jbuf) jbuf = (unsigned char *)malloc(600 * 1024);
    if (!jbuf) return;

    saving = 1;
    int len = -1;
    if (camera_jpeg_age_ms() < 400) len = camera_latest_jpeg(jbuf, 600 * 1024);
    if (len <= 0) {
        /* JPEG 太旧（或还没有）：直接用当前预览画面编码，保证"所见即所得" */
        uint64_t seq = 0;
        const Surface *pv = camera_preview(&seq);
        if (pv) {
            unsigned char *enc = NULL;
            int elen = 0;
            if (image_encode_jpeg_mem(pv, 88, &enc, &elen) == 0) {
                len = elen;
                if (elen <= 600 * 1024) memcpy(jbuf, enc, elen);
                free(enc);
            }
        }
    }
    saving = 0;
    if (len <= 0) {
        ui_toast("拍照失败，请稍后重试");
        return;
    }

    char path[320];
    if (media_new_path(MEDIA_PHOTO, path, sizeof(path)) != 0) {
        ui_toast("文件名生成失败");
        return;
    }
    if (file_write_all(path, jbuf, len) != 0) {
        ui_toast("写入失败，请检查存储");
        return;
    }
    snprintf(last_photo, sizeof(last_photo), "%s", path);
    refresh_last_thumb();
    shot_count++;

    /* 闪屏 + 提示 */
    flash_t0 = now_ms();
    flash_until = (int)(flash_t0 + 260);
    const char *base = strrchr(path, '/');
    char msg[96];
    snprintf(msg, sizeof(msg), "已保存 %.60s", base ? base + 1 : path);
    ui_toast(msg);
}

/* ---------------- 绘制 ---------------- */
static void draw_preview(Surface *s)
{
    uint64_t seq = 0;
    const Surface *pv = camera_preview(&seq);
    if (pv && seq != 0) {
        last_seq = seq;
        gfx_blit_cover(s, 0, 0, SCREEN_W, SCREEN_H, pv, 255);
    } else {
        gfx_fill_rect(s, 0, 0, SCREEN_W, SCREEN_H, RGB(0x10, 0x14, 0x1a));
        const char *msg = camera_no_signal_ms() > 2500
                        ? "摄像头无响应，正在重试…（可重新插拔）"
                        : "正在启动摄像头…";
        text_draw_center(s, SCREEN_W / 2, SCREEN_H / 2 - 20, msg, FONT_TITLE, C_TEXT_DIM);
    }
}

static void draw_top_bar(Surface *s)
{
    gfx_vgradient_a(s, 0, 0, SCREEN_W, 92, RGB(0, 0, 0), RGB(0, 0, 0), 0);
    for (int i = 0; i < 92; i++) {
        int a = 150 - i * 150 / 92;
        gfx_fill_rect_a(s, 0, i, SCREEN_W, 1, RGB(0, 0, 0), a);
    }
    /* 返回 */
    gfx_fill_circle_a(s, 32, 30, 20, RGB(0, 0, 0), 90);
    icon_draw_cached(s, IC_BACK, 32, 30, 22, C_WHITE);
    /* 标题与状态：两行排布 */
    text_draw(s, 64, 6, "拍照", FONT_TITLE, C_WHITE);
    char sub[128];
    if (camera_source() == CAM_SRC_V4L2) {
        snprintf(sub, sizeof(sub), camera_fps() > 0 ? "%s · %d fps" : "%s · 启动中",
                 camera_device_path(), camera_fps());
    } else {
        snprintf(sub, sizeof(sub), "演示画面 · 未检测到摄像头");
    }
    text_draw(s, 66, 36, sub, FONT_SMALL, RGB(0xd8, 0xe6, 0xe0));

    /* 右上角参数小标签 */
    int label = camera_source() == CAM_SRC_V4L2 ? (camera_pixel_format() == 0x47504a4d ? 1 : 0) : -1;
    const char *fmt = label == 1 ? "MJPEG" : (label == 0 ? "YUYV" : "DEMO");
    int w = ui_pill_width(fmt);
    ui_pill(s, SCREEN_W - w - 18, 20, fmt, RGB(0, 0, 0), C_WHITE);
    gfx_fill_round_rect_a(s, SCREEN_W - w - 18, 20, w, 28, 14, RGB(0, 0, 0), 90);
    text_draw_vcenter_center(s, SCREEN_W - w / 2 - 18, 20, 28, fmt, FONT_SMALL, C_WHITE);
}

static void draw_bottom_bar(Surface *s)
{
    int y0 = SCREEN_H - BAR_H;
    for (int i = 0; i < BAR_H; i++) {
        int a = i * 165 / BAR_H;
        gfx_fill_rect_a(s, 0, y0 + i, SCREEN_W, 1, RGB(0, 0, 0), a);
    }
    /* 模式文字 */
    text_draw_center(s, SCREEN_W / 2, y0 + 12, "拍照", FONT_BODY, RGB(0xf2, 0xf6, 0xf4));

    /* 左：最近照片缩略图 */
    int tx = THUMB_X, ty = THUMB_Y;
    if (last_thumb) {
        for (int y = 0; y < THUMB_SIZE; y++) {
            for (int x = 0; x < THUMB_SIZE; x++) {
                /* 圆角遮罩 */
                int dx = x - THUMB_SIZE / 2, dy = y - THUMB_SIZE / 2;
                int rr = THUMB_SIZE / 2 - 2;
                if (dx * dx + dy * dy > rr * rr) continue;
                int sx = x * last_thumb->w / THUMB_SIZE;
                int sy = y * last_thumb->h / THUMB_SIZE;
                gfx_pixel(s, tx + x, ty + y, last_thumb->px[(size_t)sy * last_thumb->stride + sx]);
            }
        }
        gfx_circle_outline_a(s, tx + THUMB_SIZE / 2, ty + THUMB_SIZE / 2, THUMB_SIZE / 2, 2,
                             RGB(0xff, 0xff, 0xff), 160);
    } else {
        gfx_fill_circle_a(s, tx + THUMB_SIZE / 2, ty + THUMB_SIZE / 2, THUMB_SIZE / 2, RGB(0x25, 0x2c, 0x36), 220);
        icon_draw_cached(s, IC_IMAGE, tx + THUMB_SIZE / 2, ty + THUMB_SIZE / 2, 26, C_TEXT_DIM);
    }
    text_draw_center(s, tx + THUMB_SIZE / 2, ty + THUMB_SIZE + 4, "图库", FONT_SMALL, C_TEXT_DIM);

    /* 中：快门 */
    int r = SHUTTER_R;
    if (shutter_pressed) r = SHUTTER_R - 4;
    gfx_fill_circle_a(s, SHUTTER_CX, SHUTTER_CY, r + 6, RGB(0xff, 0xff, 0xff), 70);
    gfx_fill_circle(s, SHUTTER_CX, SHUTTER_CY, r, RGB(0xf5, 0xf8, 0xf7));
    gfx_fill_circle(s, SHUTTER_CX, SHUTTER_CY, r - 6, RGB(0xd8, 0xde, 0xdc));
    gfx_fill_circle(s, SHUTTER_CX, SHUTTER_CY, r - 14, RGB(0xff, 0xff, 0xff));

    /* 右：存储信息 */
    int free_mb = media_free_mb();
    char info[64];
    if (free_mb >= 1024) snprintf(info, sizeof(info), "%d.%d GB", free_mb / 1024, (free_mb % 1024) * 10 / 1024);
    else snprintf(info, sizeof(info), "%d MB", free_mb);
    int rx = SCREEN_W - 24;
    text_draw_right(s, rx, y0 + 26, info, FONT_BODY, C_WHITE);
    char cnt[64];
    long shots = (long)free_mb * 1024L / 300L;
    if (shots > 9999) shots = 9999;
    snprintf(cnt, sizeof(cnt), "约可存 %ld 张", shots);
    text_draw_right(s, rx, y0 + 54, cnt, FONT_SMALL, C_TEXT_DIM);
    text_draw_right(s, rx, y0 + 76, "内置存储", FONT_SMALL, C_TEXT_MUTED);
}

static void draw_focus(Surface *s)
{
    if (!focus_until || (int)now_ms() > focus_until) return;
    uint64_t dt = now_ms() - focus_t0;
    float k = (float)dt / 700.0f;
    if (k > 1) return;
    int base = 30 + (int)(22 * (1.0f - ease_out_cubic(k)));   /* 从大到小收缩 */
    int half = base;
    int a = (int)(220 * (1.0f - k));
    uint32_t c = RGB(0xff, 0xe0, 0x60);
    int x = focus_x - half, y = focus_y - half, sz = half * 2;
    int len = sz / 3;
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    if (x + sz > SCREEN_W - 4) sz = SCREEN_W - 4 - x;
    if (y + sz > SCREEN_H - 4) sz = SCREEN_H - 4 - y;
    /* 四个角 */
    for (int i = 0; i < len; i++) {
        gfx_pixel_a(s, x + i, y, c, a);
        gfx_pixel_a(s, x, y + i, c, a);
        gfx_pixel_a(s, x + sz - i, y, c, a);
        gfx_pixel_a(s, x + sz, y + i, c, a);
        gfx_pixel_a(s, x + i, y + sz, c, a);
        gfx_pixel_a(s, x, y + sz - i, c, a);
        gfx_pixel_a(s, x + sz - i, y + sz, c, a);
        gfx_pixel_a(s, x + sz, y + sz - i, c, a);
    }
}

void app_camera_frame(uint64_t t_ms)
{
    Surface *s = gfx_back();
    if (!s) return;
    camera_rescan();
    int cam_ev = camera_take_event();
    if (cam_ev == CAM_EV_ACQUIRED) ui_toast("已检测到摄像头，切换到实时画面");
    else if (cam_ev == CAM_EV_FELL_BACK) ui_toast("摄像头无响应，已切回演示画面（可重新插拔）");
    gfx_reset_clip();
    draw_preview(s);
    draw_top_bar(s);
    draw_bottom_bar(s);
    draw_focus(s);

    /* 闪屏动画 */
    if (flash_until && (int)t_ms < flash_until) {
        float k = 1.0f - (float)((int)t_ms - flash_t0) / 260.0f;
        int a = (int)(210 * fclampf(k, 0.0f, 1.0f));
        gfx_fill_rect_a(s, 0, 0, SCREEN_W, SCREEN_H, C_WHITE, a);
    }
    if (stat_t0 == 0) stat_t0 = t_ms;
    (void)stat_t0;
    (void)saving;
}

int app_camera_event(const UiEvent *e)
{
    if (e->type == UI_EV_DOWN) {
        if (ui_hit(e->x, e->y, SHUTTER_CX - SHUTTER_R - 12, SHUTTER_CY - SHUTTER_R - 12,
                   (SHUTTER_R + 12) * 2, (SHUTTER_R + 12) * 2)) {
            shutter_pressed = 1;
        }
        return 0;
    }
    if (e->type == UI_EV_UP) {
        int was = shutter_pressed;
        shutter_pressed = 0;
        if (e->tap) {
            /* 快门 */
            if (was || ui_hit(e->x, e->y, SHUTTER_CX - SHUTTER_R - 12, SHUTTER_CY - SHUTTER_R - 12,
                              (SHUTTER_R + 12) * 2, (SHUTTER_R + 12) * 2)) {
                do_capture();
                return 0;
            }
            /* 返回 */
            if (ui_topbar_back_hit(e->x, e->y) || ui_hit(e->x, e->y, 8, 8, 48, 52))
                return 1;
            /* 左下角缩略图 -> 图库 */
            if (ui_hit(e->x, e->y, THUMB_X - 8, THUMB_Y - 8, THUMB_SIZE + 16, THUMB_SIZE + 30)) {
                app_open(APP_GALLERY);
                return 0;
            }
            /* 其它区域：对焦框 */
            if (e->y > 60 && e->y < SCREEN_H - BAR_H - 10) {
                focus_x = e->x;
                focus_y = e->y;
                focus_t0 = now_ms();
                focus_until = (int)(focus_t0 + 700);
            }
        }
        return 0;
    }
    return 0;
}

const AppDef g_app_camera = {
    .title = "拍照",
    .en = "Camera",
    .icon = APPICON_CAMERA,
    .needs_camera = 1,
    .enter = app_camera_enter,
    .leave = app_camera_leave,
    .frame = app_camera_frame,
    .event = app_camera_event
};
