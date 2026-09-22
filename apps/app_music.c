/*
 * app_music.c — 音乐播放器界面（shixi 栈）
 *
 * 后端是 `core/music_player.c`（mplayer -slave + 命名管道，原 LVGL 项目搬运），
 * 这里只做「界面 + 交互映射」：
 *
 *   - 进度条：**松手才 seek**（沿用原项目的做法，避免拖动时连续发 seek 命令）
 *   - 音量条：按住即生效（音量改变是廉价且能立刻听到的反馈）
 *   - 离开应用**不停止播放**（mplayer 是独立子进程）；要停按「停止」或退出程序
 *   - `MP_Poll()` 由 main.c 主循环驱动，所以切屏后仍在收应答、播完自动下一首
 *
 * 降级：没有 mp3 → 状态显示「没有找到 mp3」并把按钮画灰；mplayer 起不来
 * （宿主模拟器上通常没有）→ 起播 1.2 秒后仍无时长答复即判定为「播放器不可用」。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "apps.h"
#include "ui.h"
#include "media.h"
#include "util.h"
#include "music_player.h"

/* ---------------- 布局（800×480） ---------------- */
#define COVER_X   36
#define COVER_Y   84
#define COVER_S   200
#define INFO_X    260
#define INFO_W    500

#define BAR_X     36
#define BAR_W     728
#define PROG_Y    306
#define PROG_H    12
#define PROG_BAND_Y0 282
#define PROG_BAND_Y1 332

#define VOL_LABEL_W 84
#define VOL_X     120
#define VOL_W     520
#define VOL_Y     350
#define VOL_H     10
#define VOL_BAND_Y0 336
#define VOL_BAND_Y1 376

#define BTN_W     88
#define BTN_H     60
#define BTN_GAP   24
#define BTN_X0    188          /* 4 个按钮 88+24*3 居中：(800-424)/2 */
#define BTN_Y     400
#define PLAY_CX   (BTN_X0 + (BTN_W + BTN_GAP) + BTN_W / 2)   /* 第 2 个按钮中心 */
#define PLAY_CY   (BTN_Y + BTN_H / 2)
#define PLAY_R    34

/* ---------------- 状态 ---------------- */
static int inited, failed, want_play;
static int dragging_prog, dragging_vol, drag_sec;
static int vol = 60;
static uint64_t play_t;
static char dir_str[512];   /* 实际使用的曲目目录（界面上要提示） */

static int btn_x(int i) { return BTN_X0 + i * (BTN_W + BTN_GAP); }

static const char *status_text(void)
{
    if (failed) return "播放器不可用";
    if (MP_TrackCount() <= 0) return "没有找到 mp3";
    if (MP_Playing()) return MP_Paused() ? "已暂停" : "播放中";
    return "未播放";
}

static void mmss(char *buf, int n, int sec)
{
    if (sec < 0) sec = 0;
    snprintf(buf, n, "%d:%02d", sec / 60, sec % 60);
}

/* ---------------- 绘制 ---------------- */
static void draw_cover(Surface *s)
{
    int cx = COVER_X + COVER_S / 2, cy = COVER_Y + COVER_S / 2;
    gfx_fill_round_rect(s, COVER_X, COVER_Y, COVER_S, COVER_S, 22, C_SURFACE);
    gfx_fill_circle(s, cx, cy, 76, C_SURFACE2);        /* 唱片 */
    gfx_fill_circle(s, cx, cy, 74, C_BG);
    gfx_fill_circle(s, cx, cy, 30, MP_Paused() ? C_TEXT_MUTED : C_ACCENT);
    gfx_fill_circle(s, cx, cy, 8, C_BG);
    gfx_circle_outline_a(s, cx, cy, 58, 1, C_TEXT, 26);
    gfx_circle_outline_a(s, cx, cy, 46, 1, C_TEXT, 20);
}

static void draw_transport(Surface *s)
{
    int have = MP_TrackCount() > 0 && !failed;
    uint32_t bg = have ? C_SURFACE2 : C_SURFACE;
    uint32_t fg = have ? C_TEXT : C_TEXT_MUTED;

    /* 上一首 / 下一首 / 停止 */
    ui_button(s, btn_x(0), BTN_Y, BTN_W, BTN_H, "", bg, fg, 0, 16);
    icon_draw(s, IC_CHEVRON_L, btn_x(0) + BTN_W / 2, BTN_Y + BTN_H / 2, 26, fg);

    ui_button(s, btn_x(2), BTN_Y, BTN_W, BTN_H, "", bg, fg, 0, 16);
    icon_draw(s, IC_CHEVRON_R, btn_x(2) + BTN_W / 2, BTN_Y + BTN_H / 2, 26, fg);

    ui_button(s, btn_x(3), BTN_Y, BTN_W, BTN_H, "", bg, fg, 0, 16);
    gfx_fill_round_rect(s, btn_x(3) + BTN_W / 2 - 8, BTN_Y + BTN_H / 2 - 8, 16, 16, 3, fg);

    /* 播放 / 暂停（主按钮） */
    gfx_fill_circle(s, PLAY_CX, PLAY_CY, PLAY_R, have ? C_ACCENT : C_SURFACE2);
    icon_draw(s, MP_Playing() && !MP_Paused() ? IC_PAUSE : IC_PLAY,
              PLAY_CX, PLAY_CY, 30, have ? C_BLACK : C_TEXT_MUTED);
}

static void draw_bars(Surface *s)
{
    int dur = MP_Duration();
    int pos = dragging_prog ? drag_sec : MP_Position();
    char tb[32];

    /* 进度条 */
    mmss(tb, sizeof tb, pos);
    text_draw_vcenter(s, BAR_X, PROG_BAND_Y0, 26, tb, FONT_SMALL, C_TEXT_DIM);
    mmss(tb, sizeof tb, dur);
    int dw = text_width(FONT_SMALL, tb);
    text_draw_vcenter(s, BAR_X + BAR_W - dw, PROG_BAND_Y0, 26, tb, FONT_SMALL, C_TEXT_DIM);

    gfx_fill_round_rect(s, BAR_X, PROG_Y, BAR_W, PROG_H, PROG_H / 2, C_SURFACE2);
    if (dur > 0) {
        int filled = BAR_W * pos / dur;
        if (filled < 0) filled = 0;
        if (filled > BAR_W) filled = BAR_W;
        if (filled > 4) gfx_fill_round_rect(s, BAR_X, PROG_Y, filled, PROG_H, PROG_H / 2, C_ACCENT);
        gfx_fill_circle(s, BAR_X + filled, PROG_Y + PROG_H / 2, 9, C_TEXT);
        gfx_fill_circle(s, BAR_X + filled, PROG_Y + PROG_H / 2, 5, C_ACCENT);
    }

    /* 音量条 */
    text_draw_vcenter(s, BAR_X, VOL_BAND_Y0, 40, "音量", FONT_SMALL, C_TEXT_DIM);
    gfx_fill_round_rect(s, VOL_X, VOL_Y, VOL_W, VOL_H, VOL_H / 2, C_SURFACE2);
    int vw = VOL_W * vol / 100;
    if (vw > 4) gfx_fill_round_rect(s, VOL_X, VOL_Y, vw, VOL_H, VOL_H / 2, C_BLUE);
    gfx_fill_circle(s, VOL_X + vw, VOL_Y + VOL_H / 2, 7, C_TEXT);
    snprintf(tb, sizeof tb, "%d%%", vol);
    dw = text_width(FONT_SMALL, tb);
    text_draw_vcenter(s, VOL_X + VOL_W + 20, VOL_BAND_Y0, 40, tb, FONT_SMALL, C_TEXT_DIM);
}

static void app_music_frame(uint64_t t_ms)
{
    Surface *s = gfx_back();
    if (!s) return;
    gfx_reset_clip();
    gfx_fill_rect(s, 0, 0, SCREEN_W, SCREEN_H, C_BG);

    /* 起播失败判定：想播、但 1.2 秒后仍然既不在播也不知道时长 → mplayer 没起来 */
    if (want_play && t_ms - play_t > 1200 && !MP_Playing() && MP_Duration() == 0) {
        failed = 1;
        want_play = 0;
    }
    if (want_play && MP_Duration() > 0) want_play = 0;

    int n = MP_TrackCount();
    char sub[96];
    snprintf(sub, sizeof sub, "%s · 共 %d 首", status_text(), n);
    ui_topbar(s, "音乐", sub, 1);

    draw_cover(s);

    /* 曲目信息 */
    if (n > 0) {
        const char *name = MP_TrackName(MP_Index());
        text_draw_ellipsis(s, INFO_X, 96, name, FONT_TITLE, C_TEXT, INFO_W);
        char idx[48];
        snprintf(idx, sizeof idx, "第 %d / %d 首", MP_Index() + 1, n);
        text_draw_vcenter(s, INFO_X, 138, 28, idx, FONT_SMALL, C_TEXT_DIM);
        text_draw_vcenter(s, INFO_X, 168, 28, "轻触进度条跳转 · 轻触音量条调节",
                          FONT_SMALL, C_TEXT_MUTED);
    } else {
        text_draw_ellipsis(s, INFO_X, 96, "（没有曲目）", FONT_TITLE, C_TEXT_MUTED, INFO_W);
        char hint[768];
        snprintf(hint, sizeof hint, "把 mp3 放到 %s", dir_str[0] ? dir_str : "/root");
        text_draw_ellipsis(s, INFO_X, 146, hint, FONT_SMALL, C_TEXT_DIM, INFO_W);
    }
    if (failed)
        text_draw_ellipsis(s, INFO_X, 168, "需要板上 /bin/mplayer（本机没有就放一个同名程序到 PATH）",
                           FONT_SMALL, C_DANGER, INFO_W);

    draw_bars(s);
    draw_transport(s);

    if (dragging_prog)
        text_draw_vcenter_center(s, SCREEN_W / 2, PROG_BAND_Y1 - 4, 24, "松手跳到此处",
                                 FONT_SMALL, C_ACCENT);
    (void)t_ms;
}

/* ---------------- 交互 ---------------- */
static int bar_sec(int x)
{
    int dur = MP_Duration();
    int sec = 0;
    if (x < BAR_X) x = BAR_X;
    if (x > BAR_X + BAR_W) x = BAR_X + BAR_W;
    if (dur > 0) sec = (x - BAR_X) * dur / BAR_W;
    return sec;
}

static int vol_from_x(int x)
{
    if (x < VOL_X) x = VOL_X;
    if (x > VOL_X + VOL_W) x = VOL_X + VOL_W;
    return (x - VOL_X) * 100 / VOL_W;
}

static void do_play_pause(void)
{
    if (MP_TrackCount() <= 0 || failed) return;
    if (!MP_Playing()) {
        MP_Play(MP_Index());
        want_play = 1;
        play_t = now_ms();
    } else {
        MP_TogglePause();
    }
}

static int app_music_event(const UiEvent *e)
{
    if (e->type == UI_EV_DOWN) {
        if (e->y >= PROG_BAND_Y0 && e->y < PROG_BAND_Y1) {
            dragging_prog = 1;
            drag_sec = bar_sec(e->x);
        } else if (e->y >= VOL_BAND_Y0 && e->y < VOL_BAND_Y1 && e->x >= VOL_X - 20) {
            dragging_vol = 1;
            vol = vol_from_x(e->x);
            MP_SetVolume(vol);
        }
        return 0;
    }
    if (e->type == UI_EV_MOVE) {
        if (dragging_prog) drag_sec = bar_sec(e->x);
        else if (dragging_vol) { vol = vol_from_x(e->x); MP_SetVolume(vol); }
        return 0;
    }
    if (e->type == UI_EV_UP) {
        if (dragging_prog) {                       /* 松手才 seek */
            dragging_prog = 0;
            MP_Seek(drag_sec);
        }
        dragging_vol = 0;
        if (!e->tap) return 0;
        if (ui_topbar_back_hit(e->x, e->y)) return 1;

        int have = MP_TrackCount() > 0 && !failed;
        int x = PLAY_CX - PLAY_R, y = PLAY_CY - PLAY_R;
        if (have && ui_hit(e->x, e->y, x, y, PLAY_R * 2, PLAY_R * 2)) {
            do_play_pause();
        } else if (have && ui_hit(e->x, e->y, btn_x(0), BTN_Y, BTN_W, BTN_H)) {
            MP_Prev(); want_play = 1; play_t = now_ms();
        } else if (have && ui_hit(e->x, e->y, btn_x(2), BTN_Y, BTN_W, BTN_H)) {
            MP_Next(); want_play = 1; play_t = now_ms();
        } else if (have && ui_hit(e->x, e->y, btn_x(3), BTN_Y, BTN_W, BTN_H)) {
            MP_Stop(); want_play = 0;
        }
        return 0;
    }
    return 0;
}

static void app_music_enter(void)
{
    if (!inited) {
        const char *dir = getenv("MP_DIR");
        const char *fifo = getenv("MP_FIFO");
        char fifo_buf[512];
        struct stat st;

        if (!dir || !*dir) {
            /* 没有 MP_DIR 时：优先 $SHIXI_ROOT/music，没有就用 /root（板子上现成有 mp3） */
            snprintf(dir_str, sizeof dir_str, "%s/music", media_root());
            dir = (stat(dir_str, &st) == 0 && S_ISDIR(st.st_mode)) ? dir_str : "/root";
        }
        snprintf(dir_str, sizeof dir_str, "%s", dir);

        if (!fifo || !*fifo) {
            snprintf(fifo_buf, sizeof fifo_buf, "%s/tmp/mp_fifo", media_root());
            fifo = fifo_buf;
        }
        if (MP_Init(dir_str, fifo) != 0) failed = 1;
        inited = 1;
        vol = MP_Volume();
    }
    want_play = 0;
    dragging_prog = dragging_vol = 0;
}

static void app_music_leave(void)
{
    /* 刻意什么都不做：离开界面继续播放（要停请按「停止」） */
}

const AppDef g_app_music = {
    .title = "音乐",
    .en = "Music",
    .icon = APPICON_MUSIC,
    .needs_camera = 0,
    .enter = app_music_enter,
    .leave = app_music_leave,
    .frame = app_music_frame,
    .event = app_music_event
};
