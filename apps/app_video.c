/*
 * app_video.c — 录像应用
 *
 * 边录边把 MJPEG 帧写进 AVI（自己实现的封装器，见 core/avi.c），
 * 停止时回填索引并改名成正式文件，电脑/板子播放器都能直接播放。
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

#define BAR_H        116
#define REC_R        40
#define REC_CX       (SCREEN_W / 2)
#define REC_CY       (SCREEN_H - BAR_H / 2 - 6)
#define REC_FPS      15
#define MAX_SECONDS  120
#define THUMB_SIZE   62
#define THUMB_X      24
#define THUMB_Y      (SCREEN_H - BAR_H / 2 - 6 - THUMB_SIZE / 2)

static int      recording = 0;
static AviWriter *writer = NULL;
static uint64_t rec_t0 = 0;
static uint64_t last_add = 0;
static uint64_t last_seq = 0;        /* 预览用 */
static uint64_t rec_last_seq = 0;    /* 录像用（与预览分离，否则永远认为"没有新帧"） */
static int      rec_frames = 0;
static char     tmp_path[320] = {0};
static int      pressed = 0;
static char     last_video[320] = {0};
static Surface *last_thumb = NULL;
static int      saving = 0;
static unsigned char *jbuf = NULL;

static void refresh_last_thumb(void)
{
    if (last_thumb) { gfx_surface_free(last_thumb); last_thumb = NULL; }
    if (!last_video[0]) return;
    AviReader *r = avi_open(last_video);
    if (!r) return;
    int len = 0;
    const unsigned char *jpeg = avi_frame(r, 0, &len);
    if (jpeg) {
        Surface *full = image_load_mem(jpeg, len);
        if (full) {
            last_thumb = image_thumbnail(full, 120, 120);
            gfx_surface_free(full);
        }
    }
    avi_close(r);
}

void app_video_enter(void)
{
    ui_toast_set_bottom(150);
    last_seq = 0;
    if (!jbuf) jbuf = (unsigned char *)malloc(600 * 1024);
    if (!last_video[0]) {
        MediaItem items[4];
        int n = media_scan(items, 4);
        for (int i = 0; i < n; i++) {
            if (items[i].type == MEDIA_VIDEO) {
                snprintf(last_video, sizeof(last_video), "%s", items[i].path);
                break;
            }
        }
    }
    if (last_video[0]) refresh_last_thumb();
}

void app_video_leave(void)
{
    if (recording) {
        /* 离开应用时自动保存，避免丢录像 */
        avi_writer_close(writer, 0);
        writer = NULL;
        recording = 0;
    }
    if (last_thumb) { gfx_surface_free(last_thumb); last_thumb = NULL; }
}

static void start_recording(void)
{
    if (recording) return;
    if (media_free_mb() < 15) {
        ui_toast("存储空间不足，无法录像");
        return;
    }
    if (media_new_temp_path(tmp_path, sizeof(tmp_path)) != 0) {
        ui_toast("无法创建录像文件");
        return;
    }
    int w = CAM_FRAME_W, h = CAM_FRAME_H;
    uint64_t seq;
    const Surface *pv = camera_preview(&seq);
    if (pv) { w = pv->w; h = pv->h; }
    writer = avi_writer_open(tmp_path, w, h, REC_FPS);
    if (!writer) {
        ui_toast("录像初始化失败");
        return;
    }
    recording = 1;
    rec_frames = 0;
    rec_t0 = now_ms();
    last_add = 0;
    rec_last_seq = 0;      /* 从下一帧开始记录 */
    ui_toast("开始录像");
}

static void stop_recording(void)
{
    if (!recording || !writer) return;
    recording = 0;
    saving = 1;
    uint64_t dur_ms = now_ms() - rec_t0;
    int frames = avi_writer_frames(writer);
    int real_fps = REC_FPS;
    if (dur_ms > 300 && frames > 1) {
        real_fps = (int)((frames * 1000.0) / (double)dur_ms + 0.5);
        if (real_fps < 5) real_fps = 5;      /* 太低的帧率播放会很卡，兜底 */
        if (real_fps > 30) real_fps = 30;
    }
    avi_writer_set_fps(writer, real_fps);
    int ok = (frames > 0) ? 0 : -1;
    if (ok == 0) ok = avi_writer_close(writer, 0);
    else avi_writer_close(writer, 1);
    writer = NULL;
    saving = 0;

    if (ok != 0 || frames <= 0) {
        ui_toast("录像时间太短，未保存");
        return;
    }
    char final_path[320];
    if (media_commit_video(tmp_path, final_path, sizeof(final_path)) != 0) {
        ui_toast("保存失败，请检查存储");
        return;
    }
    snprintf(last_video, sizeof(last_video), "%s", final_path);
    refresh_last_thumb();
    const char *base = strrchr(final_path, '/');
    char msg[128];
    int dur_s = (int)((dur_ms + 500) / 1000);
    snprintf(msg, sizeof(msg), "已保存 %.48s（%d 秒 %d 帧 %d fps）",
             base ? base + 1 : final_path, dur_s < 1 ? 1 : dur_s, frames, real_fps);
    ui_toast(msg);
}

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

static void draw_top(Surface *s)
{
    for (int i = 0; i < 92; i++)
        gfx_fill_rect_a(s, 0, i, SCREEN_W, 1, RGB(0, 0, 0), 150 - i * 150 / 92);
    gfx_fill_circle_a(s, 32, 30, 20, RGB(0, 0, 0), 90);
    icon_draw_cached(s, IC_BACK, 32, 30, 22, C_WHITE);
    text_draw(s, 64, 6, "录像", FONT_TITLE, C_WHITE);

    if (recording) {
        /* 计时器：闪烁红点 + mm:ss */
        int sec = (int)((now_ms() - rec_t0) / 1000);
        char tb[32];
        snprintf(tb, sizeof(tb), "%02d:%02d", sec / 60, sec % 60);
        int tw = text_width(FONT_TITLE, tb);
        int boxw = tw + 74;
        int bx = (SCREEN_W - boxw) / 2;
        int blink = ((now_ms() / 500) & 1);
        gfx_fill_round_rect_a(s, bx, 16, boxw, 40, 20, RGB(0, 0, 0), 120);
        gfx_fill_circle_a(s, bx + 22, 36, 8, C_DANGER, blink ? 255 : 60);
        text_draw_vcenter(s, bx + 40, 16, 40, tb, FONT_TITLE, C_WHITE);
        char info[64];
        snprintf(info, sizeof(info), "%d 帧 · %d fps", rec_frames, camera_fps() > 0 ? camera_fps() : REC_FPS);
        int iw = text_width(FONT_SMALL, info);
        text_draw_vcenter(s, SCREEN_W - iw - 18, 16, 40, info, FONT_SMALL, RGB(0xff, 0xd0, 0xd0));
    } else {
        char sub[128];
        if (camera_source() == CAM_SRC_V4L2)
            snprintf(sub, sizeof(sub), camera_fps() > 0 ? "%s · %d fps · 点击红键开始录像"
                                                  : "%s · 启动中 · 点击红键开始录像",
                 camera_device_path(), camera_fps());
        else
            snprintf(sub, sizeof(sub), "演示画面（未检测到摄像头）");
        text_draw(s, 66, 36, sub, FONT_SMALL, RGB(0xd8, 0xe6, 0xe0));
    }
}

static void draw_bottom(Surface *s)
{
    int y0 = SCREEN_H - BAR_H;
    for (int i = 0; i < BAR_H; i++)
        gfx_fill_rect_a(s, 0, y0 + i, SCREEN_W, 1, RGB(0, 0, 0), i * 165 / BAR_H);

    text_draw_center(s, SCREEN_W / 2, y0 + 12, recording ? "正在录像…" : "录像",
                     FONT_BODY, recording ? RGB(0xff, 0xc9, 0xc9) : RGB(0xf2, 0xf6, 0xf4));

    /* 左：最近视频缩略图 */
    int tx = THUMB_X, ty = THUMB_Y;
    if (last_thumb) {
        for (int y = 0; y < THUMB_SIZE; y++) {
            for (int x = 0; x < THUMB_SIZE; x++) {
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
        gfx_fill_circle_a(s, tx + THUMB_SIZE / 2, ty + THUMB_SIZE / 2, 13, RGB(0, 0, 0), 110);
        icon_draw_cached(s, IC_PLAY, tx + THUMB_SIZE / 2, ty + THUMB_SIZE / 2, 16, C_WHITE);
    } else {
        gfx_fill_circle_a(s, tx + THUMB_SIZE / 2, ty + THUMB_SIZE / 2, THUMB_SIZE / 2, RGB(0x25, 0x2c, 0x36), 220);
        icon_draw_cached(s, IC_MOVIE, tx + THUMB_SIZE / 2, ty + THUMB_SIZE / 2, 26, C_TEXT_DIM);
    }
    text_draw_center(s, tx + THUMB_SIZE / 2, ty + THUMB_SIZE + 4, "图库", FONT_SMALL, C_TEXT_DIM);

    /* 中：录像键 */
    int r = REC_R;
    if (pressed) r = REC_R - 3;
    gfx_circle_outline_a(s, REC_CX, REC_CY, r + 7, 4, RGB(0xff, 0xff, 0xff), 235);
    if (!recording) {
        gfx_fill_circle_a(s, REC_CX, REC_CY, r, C_DANGER, 255);
    } else {
        /* 内圈由圆变方 + 呼吸效果 */
        int sq = r - 6 + (int)(2 * __builtin_sinf((float)now_ms() / 320.0f));
        gfx_fill_round_rect(s, REC_CX - sq, REC_CY - sq, sq * 2, sq * 2, 8, C_DANGER);
    }

    /* 右：录像说明 */
    int rx = SCREEN_W - 24;
    if (recording) {
        int left = MAX_SECONDS - (int)((now_ms() - rec_t0) / 1000);
        char t1[48];
        snprintf(t1, sizeof(t1), "剩余 %d 秒", left < 0 ? 0 : left);
        text_draw_right(s, rx, y0 + 34, t1, FONT_BODY, C_WHITE);
        int kb = avi_writer_bytes(writer) / 1024;
        char t2[48];
        snprintf(t2, sizeof(t2), "已写入 %d KB", kb);
        text_draw_right(s, rx, y0 + 56, t2, FONT_SMALL, C_TEXT_DIM);
        text_draw_right(s, rx, y0 + 78, "内置存储", FONT_SMALL, C_TEXT_MUTED);
    } else {
        int free_mb = media_free_mb();
        char t1[48];
        if (free_mb >= 1024) snprintf(t1, sizeof(t1), "%d.%d GB", free_mb / 1024, (free_mb % 1024) * 10 / 1024);
        else snprintf(t1, sizeof(t1), "%d MB", free_mb);
        text_draw_right(s, rx, y0 + 26, t1, FONT_BODY, C_WHITE);
        char t2[48];
        long mins = free_mb / 4;
        if (mins > 9999) mins = 9999;
        snprintf(t2, sizeof(t2), "可录 %ld 分钟", mins);
        text_draw_right(s, rx, y0 + 54, t2, FONT_SMALL, C_TEXT_DIM);
        text_draw_right(s, rx, y0 + 76, "内置存储", FONT_SMALL, C_TEXT_MUTED);
    }
}

void app_video_frame(uint64_t t_ms)
{
    Surface *s = gfx_back();
    if (!s) return;
    /* 录像中不切换信号源，避免正在写的文件尺寸变化 */
    if (!recording) {
        camera_rescan();
        int ev = camera_take_event();
        if (ev == CAM_EV_ACQUIRED) ui_toast("已检测到摄像头，切换到实时画面");
        else if (ev == CAM_EV_FELL_BACK) ui_toast("摄像头无响应，已切回演示画面（可重新插拔）");
    }
    gfx_reset_clip();
    draw_preview(s);

    /* 采集新帧 -> 写入 AVI（用 rec_last_seq，与预览互不干扰） */
    if (recording && writer) {
        uint64_t seq = 0;
        camera_preview(&seq);
        if (seq != 0 && seq != rec_last_seq) {
            rec_last_seq = seq;
            uint64_t now = now_ms();
            if (last_add == 0 || now - last_add >= (uint64_t)(1000 / REC_FPS)) {
                last_add = now;
                int len = camera_latest_jpeg(jbuf, 600 * 1024);
                if (len > 0 && avi_writer_add(writer, jbuf, len) == 0) rec_frames++;
            }
        }
        /* 超时或空间不足自动停止 */
        if ((int)((now_ms() - rec_t0) / 1000) >= MAX_SECONDS) {
            stop_recording();
            ui_toast("已达到最长录像时间，已自动保存");
        } else if (media_free_mb() < 5) {
            stop_recording();
            ui_toast("存储空间不足，已自动保存");
        }
    }

    draw_top(s);
    draw_bottom(s);

    if (saving) {
        gfx_fill_rect_a(s, 0, 0, SCREEN_W, SCREEN_H, RGB(0, 0, 0), 120);
        ui_spinner(s, SCREEN_W / 2, SCREEN_H / 2 - 16, 22, 6, C_WHITE, (float)(t_ms % 1200) / 1200.0f * 6.283f);
        text_draw_center(s, SCREEN_W / 2, SCREEN_H / 2 + 24, "正在保存视频…", FONT_BODY, C_WHITE);
    }
}

int app_video_event(const UiEvent *e)
{
    if (e->type == UI_EV_DOWN) {
        if (ui_hit(e->x, e->y, REC_CX - REC_R - 14, REC_CY - REC_R - 14,
                   (REC_R + 14) * 2, (REC_R + 14) * 2))
            pressed = 1;
        return 0;
    }
    if (e->type == UI_EV_UP) {
        int was = pressed;
        pressed = 0;
        if (!e->tap) return 0;
        if (was || ui_hit(e->x, e->y, REC_CX - REC_R - 14, REC_CY - REC_R - 14,
                          (REC_R + 14) * 2, (REC_R + 14) * 2)) {
            if (recording) stop_recording();
            else start_recording();
            return 0;
        }
        if (ui_topbar_back_hit(e->x, e->y) || ui_hit(e->x, e->y, 8, 8, 48, 52)) {
            if (recording) stop_recording();
            return 1;
        }
        if (ui_hit(e->x, e->y, THUMB_X - 8, THUMB_Y - 8, THUMB_SIZE + 16, THUMB_SIZE + 30)) {
            app_open(APP_GALLERY);
            return 0;
        }
        return 0;
    }
    return 0;
}

const AppDef g_app_video = {
    .title = "录像",
    .en = "Video",
    .icon = APPICON_VIDEO,
    .needs_camera = 1,
    .enter = app_video_enter,
    .leave = app_video_leave,
    .frame = app_video_frame,
    .event = app_video_event
};
