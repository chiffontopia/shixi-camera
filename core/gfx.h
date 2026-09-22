/*
 * gfx.h — 轻量图形层
 *
 * 直接操作 /dev/fb0（800x480，32bpp，像素格式 0x00RRGGBB），
 * 使用驱动的 FBIOPAN_DISPLAY 做双缓冲，避免画面撕裂。
 * 主机上编译时（-DSHIXI_HOST）使用内存缓冲，可导出 PNG 便于预览。
 */
#ifndef SHIXI_GFX_H
#define SHIXI_GFX_H

#include <stdint.h>

/* ---------- 颜色 ---------- */
#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

#define C_BG          RGB(0x0d, 0x11, 0x17)   /* 全局背景 */
#define C_SURFACE     RGB(0x16, 0x1c, 0x24)   /* 卡片 */
#define C_SURFACE2    RGB(0x22, 0x2a, 0x35)   /* 卡片高亮 */
#define C_LINE        RGB(0x2c, 0x35, 0x42)   /* 分隔线 */
#define C_TEXT        RGB(0xff, 0xff, 0xff)
#define C_TEXT_DIM    RGB(0xa9, 0xb3, 0xc0)
#define C_TEXT_MUTED  RGB(0x6d, 0x78, 0x88)
#define C_ACCENT      RGB(0x2e, 0xd3, 0xa8)   /* 主色（薄荷绿） */
#define C_ACCENT_DK   RGB(0x18, 0x9e, 0x7c)
#define C_BLUE        RGB(0x4c, 0x8d, 0xff)
#define C_DANGER      RGB(0xff, 0x5a, 0x5f)
#define C_WARN        RGB(0xff, 0xb0, 0x20)
#define C_WHITE       RGB(0xff, 0xff, 0xff)
#define C_BLACK       RGB(0x00, 0x00, 0x00)
#define C_OVERLAY     RGB(0x00, 0x00, 0x00)

/* ---------- 画布 ---------- */
typedef struct {
    uint32_t *px;   /* 像素，行优先 */
    int w, h;
    int stride;     /* 行跨度（单位：像素） */
} Surface;

#define SCREEN_W 800
#define SCREEN_H 480
#define STATUS_BAR_H 30

/* ---------- 生命周期 ---------- */
int  gfx_init(void);            /* 打开显示设备、准备双缓冲 */
void gfx_shutdown(void);
Surface *gfx_back(void);        /* 后台画布：所有绘制都画在这里 */
void gfx_present(void);         /* 提交后台画布到屏幕（双缓冲翻页） */
void gfx_set_clip(int x, int y, int w, int h);
void gfx_reset_clip(void);
/* 查询当前裁剪区（文字渲染等自绘模块需要遵守） */
void gfx_clip_rect(int *x0, int *y0, int *x1, int *y1);
int  gfx_width(void);
int  gfx_height(void);

/* 主机模拟器用：把当前后台画布存成 PNG/PPM */
#ifdef SHIXI_HOST
int  gfx_save_ppm(const char *path);
#endif

/* ---------- 位图绘制 ---------- */
uint32_t gfx_blend(uint32_t dst, uint32_t src, int a);      /* a: 0..255 */
void gfx_clear(Surface *s, uint32_t c);
void gfx_pixel(Surface *s, int x, int y, uint32_t c);
void gfx_pixel_a(Surface *s, int x, int y, uint32_t c, int a);
void gfx_fill_rect(Surface *s, int x, int y, int w, int h, uint32_t c);
void gfx_fill_rect_a(Surface *s, int x, int y, int w, int h, uint32_t c, int a);
void gfx_rect_outline(Surface *s, int x, int y, int w, int h, uint32_t c);
void gfx_fill_round_rect(Surface *s, int x, int y, int w, int h, int r, uint32_t c);
void gfx_fill_round_rect_a(Surface *s, int x, int y, int w, int h, int r, uint32_t c, int a);
void gfx_round_rect_outline(Surface *s, int x, int y, int w, int h, int r, int thick, uint32_t c);
void gfx_fill_circle(Surface *s, int cx, int cy, int r, uint32_t c);
void gfx_fill_circle_a(Surface *s, int cx, int cy, int r, uint32_t c, int a);
void gfx_circle_outline(Surface *s, int cx, int cy, int r, int thick, uint32_t c);
void gfx_circle_outline_a(Surface *s, int cx, int cy, int r, int thick, uint32_t c, int a);
void gfx_fill_triangle(Surface *s, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c);
void gfx_line(Surface *s, int x0, int y0, int x1, int y1, uint32_t c);
void gfx_line_a(Surface *s, int x0, int y0, int x1, int y1, uint32_t c, int a);
void gfx_vgradient(Surface *s, int x, int y, int w, int h, uint32_t top, uint32_t bot);
void gfx_vgradient_a(Surface *s, int x, int y, int w, int h, uint32_t top, uint32_t bot, int a);
void gfx_shadow(Surface *s, int x, int y, int w, int h, int r, int spread);

/* ---------- 图像 ---------- */
void gfx_blit(Surface *dst, int dx, int dy, const Surface *src);
void gfx_blit_a(Surface *dst, int dx, int dy, const Surface *src, int a);
void gfx_blit_scaled(Surface *dst, int dx, int dy, int dw, int dh,
                     const Surface *src, int sx, int sy, int sw, int sh, int a);
/* 把 src 按"等比缩放铺满 + 居中裁剪"的方式画到 dst 的 (dx,dy,dw,dh) 区域 */
void gfx_blit_cover(Surface *dst, int dx, int dy, int dw, int dh,
                    const Surface *src, int a);

/*
 * 带透明键的贴图：内部画布用 GX_KEY 填充表示"未绘制"，
 * 贴图时跳过这些像素（图标缓存、贴纸等离屏渲染用）。
 */
#define GX_KEY 0xFF000000u
void gfx_blit_keyed(Surface *dst, int dx, int dy, const Surface *src);
void gfx_blit_keyed_a(Surface *dst, int dx, int dy, const Surface *src, int a);

/* 画布管理（用于离屏渲染：图标缓存、缩略图等） */
Surface *gfx_surface_new(int w, int h);
Surface *gfx_surface_new_keyed(int w, int h);   /* 以透明键填充 */
void     gfx_surface_free(Surface *s);

#endif /* SHIXI_GFX_H */
