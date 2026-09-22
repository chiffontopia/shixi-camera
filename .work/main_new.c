/*
 * main.c — 程序入口、桌面（Launcher）、应用调度与切换动画
 *
 * 界面尺寸 800x480，桌面仿手机：状态栏 + 时钟 + 四个应用图标。
 * 触摸操作；无人时可打开调试通道（SHIXI_DEBUG_INPUT=1）从命名管道注入事件。
 */
#include "gfx.h"
#include "font.h"
#include "input.h"
#include "ui.h"
#include "media.h"
#include "camera.h"
#include "avi.h"
#include "image.h"
#include "util.h"
#include "apps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/file.h>
#include <time.h>

#include "music_player.h"

const AppDef *g_apps[APP_COUNT] = { &g_app_camera, &g_app_video, &g_app_gallery,
                                    &g_app_chat, &g_app_brick, &g_app_music };

/* ---------------- 全局状态 ---------------- */
static int  cur_app = -1;            /* -1 = 桌面 */
static int  quit_flag = 0;
static int  lock_fd = -1;

/* 切换动画 */
static int  trans_active = 0;
static uint64_t trans_t0 = 0;
static Surface *trans_snap = NULL;
#define TRANS_MS 240

/* 桌面图标布局：每行 3 个的多行网格（6 个应用 = 2 行 × 3 个）
 * 一行放不下（工单 20 之后已经是 5 个），所以按数量排成网格；
 * 尺寸/间距在竖直与水平两个方向各自自适应，应用再多也只是多一行。 */
#define ICON_COLS  3
#define ICON_MAX   96
#define ICON_MIN   64
#define GRID_TOP   164          /* 时钟部件之下 */
#define GRID_BOT   444          /* 底部存储/提示条之上 */
#define LABEL_H    25           /* 图标下的中文标签占位 */
static int icon_size = ICON_MAX;
static int cell_w = 140, cell_h = 145;

static void icon_layout(void)
{
    int rows = (APP_COUNT + ICON_COLS - 1) / ICON_COLS;
    int size = ICON_MAX, gx = 44, gy = 24;
    while (size > ICON_MIN) {
        int need_v = rows * (size + LABEL_H) + (rows - 1) * gy;
        int need_h = ICON_COLS * size + (ICON_COLS - 1) * gx;
        if (need_v <= GRID_BOT - GRID_TOP && need_h <= SCREEN_W - 32) break;
        size -= 4;
        if (gx > 20) gx -= 2;
        if (gy > 12) gy -= 2;
    }
    icon_size = size;
    cell_w = size + gx;
    cell_h = size + LABEL_H + gy;
}

static int icon_y(int i) { return GRID_TOP + (i / ICON_COLS) * cell_h; }

static int icon_x(int i)
{
    int total = ICON_COLS * icon_size + (ICON_COLS - 1) * (cell_w - icon_size);
    int x0 = (SCREEN_W - total) / 2;
    return x0 + (i % ICON_COLS) * cell_w;
}

/* 按下高亮状态 */
static int press_icon = -1;
static uint64_t press_t0 = 0;

static void on_signal(int sig) { (void)sig; quit_flag = 1; }

/* ---------------- 桌面背景 ---------------- */
static void draw_wallpaper(Surface *s)
{
    gfx_vgradient(s, 0, 0, s->w, s->h, RGB(0x14, 0x1d, 0x2e), RGB(0x0a, 0x0e, 0x15));
    /* 顶部柔光 */
    for (int r = 260; r > 0; r -= 6)
        gfx_fill_circle_a(s, s->w - 120, -60, r, RGB(0x2e, 0xd3, 0xa8), 4);
    for (int r = 200; r > 0; r -= 6)
        gfx_fill_circle_a(s, 90, s->h + 40, r, RGB(0x4c, 0x8d, 0xff), 4);
    /* 淡淡的装饰圆环 */
    gfx_circle_outline_a(s, s->w - 60, 130, 90, 1, RGB(0xff, 0xff, 0xff), 12);
    gfx_circle_outline_a(s, 60, 300, 130, 1, RGB(0xff, 0xff, 0xff), 8);
}

static void draw_clock_widget(Surface *s)
{
    char t[16], d[48];
    ui_time_str(t, sizeof(t));
    ui_date_str(d, sizeof(d));
    int tw = text_width(FONT_HUGE, t);
    text_draw(s, (SCREEN_W - tw) / 2, 66, t, FONT_HUGE, C_WHITE);
    int dw = text_width(FONT_BODY, d);
    text_draw(s, (SCREEN_W - dw) / 2, 118, d, FONT_BODY, C_TEXT_DIM);
}

/* ---------------- 桌面绘制 ---------------- */
static void launcher_frame(uint64_t t_ms)
{
    Surface *s = gfx_back();
    if (!s) return;
    gfx_reset_clip();
    draw_wallpaper(s);
    ui_status_bar(s, NULL);
    draw_clock_widget(s);

    /* 应用图标 */
    for (int i = 0; i < APP_COUNT; i++) {
        int x = icon_x(i);
        int y = icon_y(i);
        Surface *ic = appicon_get((AppIconId)g_apps[i]->icon, icon_size);
        int lift = 0, sc = icon_size;
        if (press_icon == i) {
            uint64_t dt = now_ms() - press_t0;
            if (dt < 140) { sc = icon_size - (int)(8 * dt / 140); lift = (int)(6 * dt / 140); }
            else { sc = icon_size - 8; lift = 6; }
        }
        if (ic) {
            int off = (icon_size - sc) / 2;
            if (sc == icon_size) {
                gfx_blit_keyed(s, x, y - lift, ic);
            } else {
                gfx_blit_scaled(s, x + off, y + off - lift, sc, sc, ic, 0, 0, ic->w, ic->h, 255);
            }
        }
        /* 标签 */
        text_draw_center(s, x + icon_size / 2, y + icon_size + 6, g_apps[i]->title,
                         FONT_BODY, press_icon == i ? C_ACCENT : C_TEXT);
    }

    /* 底部提示 + 存储信息 */
    char info[96];
    int free_mb = media_free_mb();
    if (free_mb >= 1024) snprintf(info, sizeof(info), "存储 %d.%d GB 可用", free_mb / 1024, (free_mb % 1024) * 10 / 1024);
    else snprintf(info, sizeof(info), "存储 %d MB 可用", free_mb);
    text_draw_vcenter(s, 20, SCREEN_H - 44, 30, info, FONT_SMALL, C_TEXT_MUTED);
    text_draw_vcenter(s, 0, SCREEN_H - 44, 30, "", FONT_SMALL, C_TEXT_MUTED);
    const char *hint = "轻触图标进入应用";
    int hw = text_width(FONT_SMALL, hint);
    text_draw_vcenter(s, SCREEN_W - hw - 20, SCREEN_H - 44, 30, hint, FONT_SMALL, C_TEXT_MUTED);
    (void)t_ms;
}

static int launcher_event(const UiEvent *e)
{
    ui_toast_set_bottom(96);
    if (e->type == UI_EV_DOWN) {
        press_icon = -1;
        for (int i = 0; i < APP_COUNT; i++) {
            if (ui_hit(e->x, e->y, icon_x(i), icon_y(i), icon_size, icon_size + 34)) {
                press_icon = i;
                press_t0 = now_ms();
                break;
            }
        }
        return 0;
    }
    if (e->type == UI_EV_UP) {
        int idx = press_icon;
        press_icon = -1;
        if (idx >= 0 && e->tap) {
            app_open(idx);
        }
        return 0;
    }
    if (e->type == UI_EV_MOVE) {
        if (press_icon >= 0 && !ui_hit(e->x, e->y, icon_x(press_icon), icon_y(press_icon) - 20,
                                       icon_size, icon_size + 64))
            press_icon = -1;
        return 0;
    }
    return 0;
}

/* ---------------- 切换动画 ---------------- */
static void snapshot_screen(void)
{
    Surface *s = gfx_back();
    if (!s) return;
    if (trans_snap) gfx_surface_free(trans_snap);
    trans_snap = gfx_surface_new(s->w, s->h);
    if (!trans_snap) return;
    for (int y = 0; y < s->h; y++)
        memcpy(trans_snap->px + (size_t)y * trans_snap->stride,
               s->px + (size_t)y * s->stride, (size_t)s->w * 4);
}

static void start_transition(void)
{
    snapshot_screen();
    trans_active = trans_snap ? 1 : 0;
    trans_t0 = now_ms();
}

void app_open(int app_index)
{
    if (app_index < 0 || app_index >= APP_COUNT) return;
    if (cur_app == app_index) return;
    start_transition();
    if (cur_app >= 0 && g_apps[cur_app]->leave) g_apps[cur_app]->leave();
    cur_app = app_index;
    if (g_apps[cur_app]->needs_camera) app_camera_prepare();
    if (g_apps[cur_app]->enter) g_apps[cur_app]->enter();
}

void app_go_home(void)
{
    if (cur_app < 0) return;
    start_transition();
    if (g_apps[cur_app]->leave) g_apps[cur_app]->leave();
    cur_app = -1;
}

/* ---------------- 摄像头惰性打开 ---------------- */
static int camera_ready = 0;
int app_camera_prepare(void)
{
    if (camera_ready) return 0;
    if (camera_open() == 0) { camera_ready = 1; return 0; }
    return -1;
}

/* ---------------- 主循环 ---------------- */
/* 一帧：取事件 -> 处理 -> 绘制 -> 提交 */
static void tick(void)
{
    /*
     * 一次把队列里的事件全部处理掉（第一次带 12ms 超时用来休眠，其余立即返回）。
     * 以前每帧只取一个事件：手指一抖就会产生大量 MOVE，队列越堆越长，
     * 抬起（点击）事件要等好几帧才被处理 -> 手感就是"点了没反应"。
     */
    UiEvent e;
    for (int guard = 0; guard < 32; guard++) {
        int got = input_poll(&e, guard == 0 ? 12 : 0);
        if (got != 1) break;
        if (cur_app < 0) launcher_event(&e);
        else if (g_apps[cur_app]->event && g_apps[cur_app]->event(&e)) { app_go_home(); break; }
    }

    uint64_t t = now_ms();
    /* 音乐后端：每 200ms 收一次 mplayer 应答（离开音乐界面也继续，播完自动下一首） */
    static uint64_t last_music;
    if (t - last_music >= 200) { last_music = t; MP_Poll(); }

    /* 帧节奏：切换动画 60fps，应用内 30fps，桌面 10fps（只显示时钟） */
    static uint64_t last = 0;
    uint64_t interval = trans_active ? 16 : (cur_app < 0 ? 100 : 33);
    if (t - last >= interval) {
        last = t;
        gfx_reset_clip();
        if (cur_app < 0) launcher_frame(t);
        else if (g_apps[cur_app]->frame) g_apps[cur_app]->frame(t);

        /* 切换动画：旧画面淡出并轻微上移 */
        if (trans_active && trans_snap) {
            uint64_t dt = now_ms() - trans_t0;
            float k = (float)dt / TRANS_MS;
            if (k >= 1.0f) {
                trans_active = 0;
                gfx_surface_free(trans_snap);
                trans_snap = NULL;
            } else {
                int a = (int)(255 * (1.0f - ease_out_cubic(k)));
                int dy = -(int)(14 * ease_out_cubic(k));
                gfx_blit_a(gfx_back(), 0, dy, trans_snap, a);
            }
        }
        /* 轻提示画在最上层 */
        ui_toast_draw(gfx_back(), t);
        gfx_present();
    }
}

#ifdef SHIXI_HOST
/* ---------------- 主机模拟器：脚本驱动 + 截图 ----------------
 * 脚本每行一条命令：
 *   tap X Y            点击
 *   swipe X0 Y0 X1 Y1  滑动
 *   down X Y / up X Y  按下 / 抬起
 *   frames N           渲染 N 帧（默认 6）
 *   wait MS            等待
 *   shot PATH          把当前画面存成 PNG
 *   seed NP NV         生成 NP 张示例照片与 NV 个示例视频（用演示画面）
 *   quit               退出
 */
static void sim_seed_media(int nphotos, int nvideos)
{
    unsigned char *jbuf = (unsigned char *)malloc(600 * 1024);
    if (!jbuf) return;
    for (int i = 0; i < nphotos; i++) {
        uint64_t seq = 0;
        for (int k = 0; k < 8 && camera_preview(&seq) == NULL; k++) usleep(60000);
        int len = camera_latest_jpeg(jbuf, 600 * 1024);
        if (len <= 0) {
            const Surface *pv = camera_preview(&seq);
            unsigned char *enc = NULL;
            int elen = 0;
            if (pv && image_encode_jpeg_mem(pv, 88, &enc, &elen) == 0) {
                if (elen <= 600 * 1024) { memcpy(jbuf, enc, elen); len = elen; }
                free(enc);
            }
        }
        if (len > 0) {
            char path[320];
            if (media_new_path(MEDIA_PHOTO, path, sizeof(path)) == 0)
                file_write_all(path, jbuf, len);
        }
        usleep(120000);
    }
    for (int v = 0; v < nvideos; v++) {
        char tmp[320], final[320];
        if (media_new_temp_path(tmp, sizeof(tmp)) != 0) break;
        AviWriter *w = avi_writer_open(tmp, CAM_FRAME_W, CAM_FRAME_H, 15);
        if (!w) break;
        for (int f = 0; f < 45; f++) {
            usleep(70000);
            int len = camera_latest_jpeg(jbuf, 600 * 1024);
            if (len > 0) avi_writer_add(w, jbuf, len);
        }
        avi_writer_close(w, 0);
        media_commit_video(tmp, final, sizeof(final));
    }
    free(jbuf);
}

static void sim_run_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "sim: 打不开脚本 %s\n", path); return; }
    char line[512];
    while (fgets(line, sizeof(line), f) && !quit_flag) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == 0) continue;
        char cmd[32] = {0};
        int a = 0, b = 0, c = 0, d = 0;
        int n = sscanf(p, "%31s %d %d %d %d", cmd, &a, &b, &c, &d);
        if (n < 1) continue;
        if (!strcmp(cmd, "tap")) {
            char b2[64]; snprintf(b2, sizeof(b2), "tap %d %d", a, b);
            input_inject_cmd(b2);
            for (int i = 0; i < 8; i++) { tick(); usleep(12000); }
        } else if (!strcmp(cmd, "swipe")) {
            char b2[96]; snprintf(b2, sizeof(b2), "swipe %d %d %d %d", a, b, c, d);
            input_inject_cmd(b2);
            for (int i = 0; i < 4; i++) { tick(); usleep(12000); }
        } else if (!strcmp(cmd, "down") || !strcmp(cmd, "up") || !strcmp(cmd, "long") ||
                   !strcmp(cmd, "move")) {
            char b2[64]; snprintf(b2, sizeof(b2), "%s %d %d", cmd, a, b);
            input_inject_cmd(b2);
            for (int i = 0; i < 4; i++) { tick(); usleep(12000); }
        } else if (!strcmp(cmd, "frames")) {
            int cnt = a > 0 ? a : 6;
            for (int i = 0; i < cnt; i++) { tick(); usleep(12000); }
        } else if (!strcmp(cmd, "wait")) {
            uint64_t t0 = now_ms();
            while (now_ms() - t0 < (uint64_t)a) { tick(); usleep(8000); }
        } else if (!strcmp(cmd, "shot")) {
            char outpath[300] = {0};
            if (sscanf(p, "%*s %299s", outpath) == 1 && outpath[0]) {
                tick();
                if (gfx_save_ppm(outpath) == 0)
                    fprintf(stderr, "sim: 截图 %s\n", outpath);
                else
                    fprintf(stderr, "sim: 截图失败 %s\n", outpath);
            }
        } else if (!strcmp(cmd, "seed")) {
            sim_seed_media(a > 0 ? a : 6, b > 0 ? b : 1);
            fprintf(stderr, "sim: 生成示例媒体完成\n");
        } else if (!strcmp(cmd, "quit")) {
            break;
        } else {
            fprintf(stderr, "sim: 未知命令 %s\n", cmd);
        }
    }
    fclose(f);
}
#endif

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* 时区：板子按东八区显示 */
    setenv("TZ", "CST-8", 1);
    tzset();

    /* 单实例：避免两个进程抢 /dev/fb0 */
    lock_fd = open("/tmp/shixi.lock", O_RDWR | O_CREAT, 0644);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "程序已在运行（/tmp/shixi.lock）\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (gfx_init() != 0) {
        fprintf(stderr, "初始化显示失败\n");
        return 1;
    }
    if (font_init() != 0) {
        fprintf(stderr, "初始化字体失败（可用 SHIXI_TTF 指定字体路径）\n");
#ifndef SHIXI_HOST
        /* 没有字体就画不出任何提示：刷红屏，让现场一眼看出是字体问题而不是黑屏 */
        gfx_clear(gfx_back(), RGB(0xb0, 0x00, 0x00));
        gfx_present();
#endif
        return 1;
    }
    if (media_init() != 0) {
        fprintf(stderr, "初始化存储失败\n");
    }
    media_cleanup_tmp();
    input_init();

    const char *dbg = getenv("SHIXI_DEBUG_INPUT");
    if (dbg && atoi(dbg)) {
        input_debug_enable(1);
        fprintf(stderr, "调试输入已开启：echo 'tap 400 430' > /tmp/shixi_ctrl\n");
    }

    icon_layout();
    appicon_preload(icon_size);

    fprintf(stderr, "石溪相机启动：触摸=%s 存储=%s\n",
            input_ready() ? input_device_name() : "不可用", media_root());

#ifdef SHIXI_HOST
    const char *script = getenv("SHIXI_SIM_SCRIPT");
    if (script && *script) {
        app_camera_prepare();
        usleep(300000);
        sim_run_script(script);
        MP_Deinit();
        camera_close();
        gfx_shutdown();
        if (lock_fd >= 0) close(lock_fd);
        return 0;
    }
#endif

    while (!quit_flag) tick();

    if (cur_app >= 0 && g_apps[cur_app]->leave) g_apps[cur_app]->leave();
    MP_Deinit();          /* 收掉后台 mplayer，别留孤儿进程 */
    camera_close();
    gfx_clear(gfx_back(), C_BLACK);
    gfx_present();
    gfx_shutdown();
    if (lock_fd >= 0) close(lock_fd);
    fprintf(stderr, "已退出\n");
    return 0;
}
