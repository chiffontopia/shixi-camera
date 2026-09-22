/*
 * ui.h — 通用界面控件与图标（矢量绘制，抗锯齿）
 */
#ifndef SHIXI_UI_H
#define SHIXI_UI_H

#include "gfx.h"
#include "font.h"
#include "input.h"

#define UI_RADIUS     14
#define UI_TOPBAR_H   58

static inline int ui_hit(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* ---------------- 图标 ---------------- */
typedef enum {
    IC_BACK = 0, IC_TRASH, IC_PLAY, IC_PAUSE, IC_SEND, IC_CHECK, IC_CLOSE,
    IC_FLASH, IC_FLIP, IC_SETTINGS, IC_GRID, IC_MORE, IC_CHEVRON_L, IC_CHEVRON_R,
    IC_INFO, IC_SDCARD, IC_SIGNAL, IC_BATTERY, IC_HOME, IC_IMAGE, IC_MOVIE,
    IC_SEARCH, IC_CLOCK, IC_SUN, IC_ZOOM, IC_LOCK, IC_REFRESH, IC_COUNT
} IconId;

void icon_draw(Surface *s, IconId id, int cx, int cy, int size, uint32_t color);
void icon_draw_a(Surface *s, IconId id, int cx, int cy, int size, uint32_t color, int alpha);
/* 缓存渲染：图标会先画进离屏画布，之后直接贴图（用于每帧重复出现的图标） */
void icon_draw_cached(Surface *s, IconId id, int cx, int cy, int size, uint32_t color);

/* ---------------- 应用图标（桌面用，彩色大图标） ---------------- */
typedef enum {
    APPICON_CAMERA = 0, APPICON_VIDEO, APPICON_GALLERY, APPICON_CHAT, APPICON_BRICK,
    APPICON_MUSIC, APPICON_COUNT
} AppIconId;

/* 取得缓存的图标画布（size x size），失败返回 NULL */
Surface *appicon_get(AppIconId id, int size);
void appicon_preload(int size);

/* ---------------- 控件 ---------------- */
void ui_topbar(Surface *s, const char *title, const char *subtitle, int show_back);
int  ui_topbar_back_hit(int x, int y);
void ui_button(Surface *s, int x, int y, int w, int h, const char *label,
               uint32_t bg, uint32_t fg, int pressed, int radius);
void ui_pill(Surface *s, int x, int y, const char *label, uint32_t bg, uint32_t fg);
int  ui_pill_width(const char *label);
void ui_progress(Surface *s, int x, int y, int w, int h, float p, uint32_t bg, uint32_t fg);
void ui_spinner(Surface *s, int cx, int cy, int r, int thick, uint32_t color, float phase);
void ui_dimmer(Surface *s, int alpha);
void ui_card(Surface *s, int x, int y, int w, int h, int radius);

/* 状态栏 */
void ui_status_bar(Surface *s, const char *right_text);
int  ui_status_bar_h(void);

/* 对话框（确认框）。按钮矩形固定，调用方做命中测试 */
void ui_dialog(Surface *s, const char *title, const char *msg,
               const char *ok_label, const char *cancel_label);
int  ui_dialog_ok_hit(int x, int y);
int  ui_dialog_cancel_hit(int x, int y);

/* 轻提示 */
void ui_toast(const char *msg);
void ui_toast_set_bottom(int px);   /* 提示条距屏幕底部的高度 */
void ui_toast_draw(Surface *s, uint64_t t_ms);
int  ui_toast_active(void);

/* 时间文案 */
void ui_time_str(char *buf, int n);       /* HH:MM */
void ui_date_str(char *buf, int n);       /* M月D日 星期X */
void ui_datetime_str(char *buf, int n);

#endif /* SHIXI_UI_H */
