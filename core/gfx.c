/*
 * gfx.c — 图形层实现（framebuffer + 双缓冲 + 基本图元）
 */
#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#ifdef SHIXI_HOST
#include "../third_party/stb_image_write.h"
#endif

/* ------------------------------------------------------------------ */
/* 内部状态                                                            */
/* ------------------------------------------------------------------ */
#ifndef SHIXI_HOST
static int   fb_fd = -1;
static uint32_t *fb_map = NULL;
static size_t fb_map_size = 0;
static struct fb_var_screeninfo fb_var;
#endif
static Surface bufs[2];
static int  cur = 0;          /* 当前“后台”缓冲索引 */
static int  double_buf = 0;
static int  have_fb = 0;
#ifdef SHIXI_HOST
static uint32_t *host_mem = NULL;
#endif

/* 裁剪区 */
static int clip_x0 = 0, clip_y0 = 0, clip_x1 = SCREEN_W, clip_y1 = SCREEN_H;
static int clip_on = 0;

/* ------------------------------------------------------------------ */
/* 颜色混合                                                            */
/* ------------------------------------------------------------------ */
uint32_t gfx_blend(uint32_t d, uint32_t s, int a)
{
    if (a <= 0)   return d;
    if (a >= 255) return s;
    uint32_t ia = 255 - a;
    uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
    uint32_t sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
    uint32_t t, r, g, b;
    t = sr * a + dr * ia + 128; r = (t + (t >> 8)) >> 8;
    t = sg * a + dg * ia + 128; g = (t + (t >> 8)) >> 8;
    t = sb * a + db * ia + 128; b = (t + (t >> 8)) >> 8;
    return (r << 16) | (g << 8) | b;
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */
static void surface_init(Surface *s, uint32_t *mem, int w, int h, int stride)
{
    s->px = mem; s->w = w; s->h = h; s->stride = stride;
}

int gfx_init(void)
{
#ifdef SHIXI_HOST
    host_mem = (uint32_t *)calloc((size_t)SCREEN_W * SCREEN_H, 4);
    if (!host_mem) return -1;
    surface_init(&bufs[0], host_mem, SCREEN_W, SCREEN_H, SCREEN_W);
    surface_init(&bufs[1], host_mem, SCREEN_W, SCREEN_H, SCREEN_W);
    double_buf = 0;
    have_fb = 1;
    return 0;
#else
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        fprintf(stderr, "gfx: 打开 /dev/fb0 失败: %s\n", strerror(errno));
        return -1;
    }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) < 0) {
        fprintf(stderr, "gfx: FBIOGET_VSCREENINFO 失败\n");
        close(fb_fd); fb_fd = -1;
        return -1;
    }
    int VW = fb_var.xres_virtual ? fb_var.xres_virtual : fb_var.xres;
    int VH = fb_var.yres_virtual ? fb_var.yres_virtual : fb_var.yres;
    int W = fb_var.xres, H = fb_var.yres;
    fb_map_size = (size_t)VW * VH * 4;
    fb_map = (uint32_t *)mmap(NULL, fb_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_map == MAP_FAILED) {
        fprintf(stderr, "gfx: mmap /dev/fb0 失败\n");
        fb_map = NULL; close(fb_fd); fb_fd = -1;
        return -1;
    }
    surface_init(&bufs[0], fb_map, W, H, VW);
    if (VH >= H * 2) {
        surface_init(&bufs[1], fb_map + (size_t)VW * H, W, H, VW);
        double_buf = 1;
    } else {
        surface_init(&bufs[1], fb_map, W, H, VW);
    }
    /* 双缓冲时保证两个缓冲都是干净的深色，避免启动瞬间花屏 */
    memset(fb_map, 0, fb_map_size);
    fb_var.yoffset = 0;
    fb_var.activate = FB_ACTIVATE_NOW;
    ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
    cur = 0;
    have_fb = 1;
    return 0;
#endif
}

void gfx_shutdown(void)
{
#ifndef SHIXI_HOST
    if (fb_map && fb_map != MAP_FAILED) munmap(fb_map, fb_map_size);
    if (fb_fd >= 0) close(fb_fd);
    fb_map = NULL; fb_fd = -1;
#endif
    have_fb = 0;
}

Surface *gfx_back(void) { return have_fb ? &bufs[cur] : NULL; }
int gfx_width(void)  { return have_fb ? bufs[cur].w : SCREEN_W; }
int gfx_height(void) { return have_fb ? bufs[cur].h : SCREEN_H; }

void gfx_present(void)
{
#ifndef SHIXI_HOST
    if (!have_fb || !double_buf) return;
    fb_var.yoffset = cur * bufs[0].h;
    fb_var.activate = FB_ACTIVATE_NOW;
    ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
    cur ^= 1;
#endif
}

#ifdef SHIXI_HOST
static void png_write_cb(void *ctx, void *data, int size)
{
    fwrite(data, 1, (size_t)size, (FILE *)ctx);
}

int gfx_save_ppm(const char *path)
{
    if (!have_fb) return -1;
    Surface *s = &bufs[cur];
    /* stb 期望 RGB 顺序，这里把 0x00RRGGBB 展开成字节流 */
    unsigned char *rgb = (unsigned char *)malloc((size_t)s->w * s->h * 3);
    if (!rgb) return -1;
    for (int y = 0; y < s->h; y++) {
        uint32_t *row = s->px + (size_t)y * s->stride;
        unsigned char *o = rgb + (size_t)y * s->w * 3;
        for (int x = 0; x < s->w; x++) {
            uint32_t c = row[x];
            o[x * 3 + 0] = (c >> 16) & 0xFF;
            o[x * 3 + 1] = (c >> 8) & 0xFF;
            o[x * 3 + 2] = c & 0xFF;
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(rgb); return -1; }
    int ok = stbi_write_png_to_func(png_write_cb, f, s->w, s->h, 3, rgb, s->w * 3);
    fclose(f);
    free(rgb);
    return ok ? 0 : -1;
}
#endif

/* ------------------------------------------------------------------ */
/* 裁剪                                                                */
/* ------------------------------------------------------------------ */
void gfx_set_clip(int x, int y, int w, int h)
{
    Surface *s = gfx_back();
    int sw = s ? s->w : SCREEN_W, sh = s ? s->h : SCREEN_H;
    clip_x0 = x < 0 ? 0 : x;
    clip_y0 = y < 0 ? 0 : y;
    clip_x1 = x + w > sw ? sw : x + w;
    clip_y1 = y + h > sh ? sh : y + h;
    clip_on = 1;
}

void gfx_clip_rect(int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = clip_x0;
    if (y0) *y0 = clip_y0;
    if (x1) *x1 = clip_x1;
    if (y1) *y1 = clip_y1;
}

void gfx_reset_clip(void)
{
    Surface *s = gfx_back();
    clip_x0 = 0; clip_y0 = 0;
    clip_x1 = s ? s->w : SCREEN_W;
    clip_y1 = s ? s->h : SCREEN_H;
    clip_on = 0;
}

/*
 * 计算矩形与裁剪区/画布的交集（就地修改调用的 x,y,w,h），返回 0 表示完全不可见。
 * 注意：第三、四个参数是"宽和高"，不是右下角坐标。
 */
static int clip_rect(Surface *s, int *x, int *y, int *w, int *h)
{
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (clip_on) {
        if (x0 < clip_x0) x0 = clip_x0;
        if (y0 < clip_y0) y0 = clip_y0;
        if (x1 > clip_x1) x1 = clip_x1;
        if (y1 > clip_y1) y1 = clip_y1;
    }
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 图元                                                                */
/* ------------------------------------------------------------------ */
void gfx_clear(Surface *s, uint32_t c)
{
    if (!s) return;
    for (int y = 0; y < s->h; y++) {
        uint32_t *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < s->w; x++) row[x] = c;
    }
}

void gfx_pixel(Surface *s, int x, int y, uint32_t c)
{
    if (!s || x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    if (clip_on && (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1)) return;
    s->px[(size_t)y * s->stride + x] = c;
}

void gfx_pixel_a(Surface *s, int x, int y, uint32_t c, int a)
{
    if (!s || a <= 0 || x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    if (clip_on && (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1)) return;
    uint32_t *p = &s->px[(size_t)y * s->stride + x];
    *p = gfx_blend(*p, c, a);
}

void gfx_fill_rect(Surface *s, int x, int y, int w, int h, uint32_t c)
{
    if (!s || w <= 0 || h <= 0) return;
    if (!clip_rect(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = s->px + (size_t)(y + j) * s->stride + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

void gfx_fill_rect_a(Surface *s, int x, int y, int w, int h, uint32_t c, int a)
{
    if (!s || w <= 0 || h <= 0 || a <= 0) return;
    if (a >= 255) { gfx_fill_rect(s, x, y, w, h, c); return; }
    if (!clip_rect(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = s->px + (size_t)(y + j) * s->stride + x;
        for (int i = 0; i < w; i++) row[i] = gfx_blend(row[i], c, a);
    }
}

void gfx_rect_outline(Surface *s, int x, int y, int w, int h, uint32_t c)
{
    if (!s || w <= 0 || h <= 0) return;
    gfx_fill_rect(s, x, y, w, 1, c);
    gfx_fill_rect(s, x, y + h - 1, w, 1, c);
    gfx_fill_rect(s, x, y + 1, 1, h - 2, c);
    gfx_fill_rect(s, x + w - 1, y + 1, 1, h - 2, c);
}

/* 圆角矩形：用 SDF 做抗锯齿，覆盖率 = clamp(0.5 - d, 0, 1) */
void gfx_fill_round_rect_a(Surface *s, int x, int y, int w, int h, int r, uint32_t c, int a)
{
    if (!s || w <= 0 || h <= 0 || a <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    if (r == 0) { gfx_fill_rect_a(s, x, y, w, h, c, a); return; }

    float hw = w * 0.5f, hh = h * 0.5f, fr = (float)r;
    float cx = x + hw, cy = y + hh;
    int vx0 = x, vy0 = y, vw = w, vh = h;
    if (!clip_rect(s, &vx0, &vy0, &vw, &vh)) return;
    int vx1 = vx0 + vw, vy1 = vy0 + vh;
    vx0 -= x; vy0 -= y; vx1 -= x; vy1 -= y;

    for (int j = vy0; j < vy1; j++) {
        float py = (float)(y + j) + 0.5f - cy;
        float qy = py < 0 ? -py : py;
        qy -= (hh - fr);
        if (qy < 0) qy = 0;
        uint32_t *row = s->px + (size_t)(y + j) * s->stride;
        for (int i = vx0; i < vx1; i++) {
            float px = (float)(x + i) + 0.5f - cx;
            float qx = px < 0 ? -px : px;
            qx -= (hw - fr);
            if (qx < 0) qx = 0;
            float d = qx * qx + qy * qy;
            d = d > 0 ? __builtin_sqrtf(d) : 0.0f;
            d -= fr;
            float cov = 0.5f - d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            int aa = (int)(cov * a + 0.5f);
            if (aa <= 0) continue;
            row[x + i] = gfx_blend(row[x + i], c, aa > 255 ? 255 : aa);
        }
    }
}

void gfx_fill_round_rect(Surface *s, int x, int y, int w, int h, int r, uint32_t c)
{
    gfx_fill_round_rect_a(s, x, y, w, h, r, c, 255);
}

/*
 * 圆角矩形描边：一次 SDF 扫描，覆盖率取 "到边界距离 <= thick/2" 的软边带，
 * 这样粗细均匀、抗锯齿干净。
 */
void gfx_round_rect_outline(Surface *s, int x, int y, int w, int h, int r, int thick, uint32_t c)
{
    if (!s || w <= 0 || h <= 0 || thick <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;

    float hw = w * 0.5f, hh = h * 0.5f, fr = (float)r;
    float cx = x + hw, cy = y + hh;
    float half = thick * 0.5f;
    int x0 = x - thick - 1, y0 = y - thick - 1;
    int ww = w + (thick + 1) * 2, hh2 = h + (thick + 1) * 2;
    if (!clip_rect(s, &x0, &y0, &ww, &hh2)) return;
    int x1 = x0 + ww, y1 = y0 + hh2;

    for (int j = y0; j < y1; j++) {
        float py = (float)j + 0.5f - cy;
        float qy = py < 0 ? -py : py;
        qy -= (hh - fr);
        if (qy < 0) qy = 0;
        uint32_t *row = s->px + (size_t)j * s->stride;
        for (int i = x0; i < x1; i++) {
            float px = (float)i + 0.5f - cx;
            float qx = px < 0 ? -px : px;
            qx -= (hw - fr);
            if (qx < 0) qx = 0;
            float d = qx * qx + qy * qy;
            d = d > 0 ? __builtin_sqrtf(d) : 0.0f;
            d -= fr;
            float ad = d < 0 ? -d : d;
            float cov = half + 0.5f - ad;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            int aa = (int)(cov * 255.0f + 0.5f);
            if (aa <= 0) continue;
            row[i] = gfx_blend(row[i], c, aa);
        }
    }
}

void gfx_fill_circle_a(Surface *s, int cx, int cy, int r, uint32_t c, int a)
{
    if (!s || r <= 0 || a <= 0) return;
    int x0 = cx - r - 1, y0 = cy - r - 1, ww = (r + 1) * 2 + 1, hh = (r + 1) * 2 + 1;
    if (!clip_rect(s, &x0, &y0, &ww, &hh)) return;
    int x1 = x0 + ww, y1 = y0 + hh;
    float fr = (float)r;
    for (int j = y0; j < y1; j++) {
        float dy = (float)j + 0.5f - (float)cy;
        uint32_t *row = s->px + (size_t)j * s->stride;
        for (int i = x0; i < x1; i++) {
            float dx = (float)i + 0.5f - (float)cx;
            float d = __builtin_sqrtf(dx * dx + dy * dy) - fr;
            float cov = 0.5f - d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            int aa = (int)(cov * a + 0.5f);
            if (aa <= 0) continue;
            row[i] = gfx_blend(row[i], c, aa > 255 ? 255 : aa);
        }
    }
}

void gfx_fill_circle(Surface *s, int cx, int cy, int r, uint32_t c)
{
    gfx_fill_circle_a(s, cx, cy, r, c, 255);
}

void gfx_circle_outline_a(Surface *s, int cx, int cy, int r, int thick, uint32_t c, int a)
{
    if (!s || r <= 0 || thick <= 0) return;
    float rr = (float)r, half = thick * 0.5f;
    int x0 = cx - r - thick, y0 = cy - r - thick;
    int ww = (r + thick + 1) * 2, hh = (r + thick + 1) * 2;
    if (!clip_rect(s, &x0, &y0, &ww, &hh)) return;
    int x1 = x0 + ww, y1 = y0 + hh;
    for (int j = y0; j < y1; j++) {
        float dy = (float)j + 0.5f - (float)cy;
        uint32_t *row = s->px + (size_t)j * s->stride;
        for (int i = x0; i < x1; i++) {
            float dx = (float)i + 0.5f - (float)cx;
            float d = __builtin_sqrtf(dx * dx + dy * dy) - rr;
            float ad = d < 0 ? -d : d;
            float cov = half + 0.5f - ad;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            int aa = (int)(cov * a + 0.5f);
            if (aa <= 0) continue;
            row[i] = gfx_blend(row[i], c, aa > 255 ? 255 : aa);
        }
    }
}

void gfx_circle_outline(Surface *s, int cx, int cy, int r, int thick, uint32_t c)
{
    gfx_circle_outline_a(s, cx, cy, r, thick, c, 255);
}

/* 三角形：扫描线 + 边缘覆盖率（3x3 超采样，足够平滑） */
static float tri_edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void gfx_fill_triangle(Surface *s, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c)
{
    if (!s) return;
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    minx--; miny--; maxx++; maxy++;
    int ww = maxx - minx, hh = maxy - miny;
    if (!clip_rect(s, &minx, &miny, &ww, &hh)) return;
    maxx = minx + ww; maxy = miny + hh;

    const int SS = 3;
    for (int j = miny; j < maxy; j++) {
        uint32_t *row = s->px + (size_t)j * s->stride;
        for (int i = minx; i < maxx; i++) {
            int hits = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    float px = i + (sx + 0.5f) / SS;
                    float py = j + (sy + 0.5f) / SS;
                    float w0 = tri_edge((float)x0, (float)y0, (float)x1, (float)y1, px, py);
                    float w1 = tri_edge((float)x1, (float)y1, (float)x2, (float)y2, px, py);
                    float w2 = tri_edge((float)x2, (float)y2, (float)x0, (float)y0, px, py);
                    if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) hits++;
                }
            }
            if (!hits) continue;
            row[i] = gfx_blend(row[i], c, hits * 255 / (SS * SS));
        }
    }
}

void gfx_line_a(Surface *s, int x0, int y0, int x1, int y1, uint32_t c, int a)
{
    if (!s || a <= 0) return;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx == 0 && ady == 0) { gfx_pixel_a(s, x0, y0, c, a); return; }
    /* 粗线用四边形近似，保证斜线也好看 */
    if (adx > ady) {
        for (int x = 0; x <= adx; x++) {
            int y = y0 + (int)((long)dy * x / (adx ? adx : 1));
            gfx_pixel_a(s, x0 + (dx < 0 ? -x : x), y, c, a);
        }
    } else {
        for (int y = 0; y <= ady; y++) {
            int x = x0 + (int)((long)dx * y / (ady ? ady : 1));
            gfx_pixel_a(s, x, y0 + (dy < 0 ? -y : y), c, a);
        }
    }
}

void gfx_line(Surface *s, int x0, int y0, int x1, int y1, uint32_t c)
{
    gfx_line_a(s, x0, y0, x1, y1, c, 255);
}

void gfx_vgradient_a(Surface *s, int x, int y, int w, int h, uint32_t top, uint32_t bot, int a)
{
    if (!s || w <= 0 || h <= 0 || a <= 0) return;
    int x0 = x, y0 = y, ww = w, hh = h;
    if (!clip_rect(s, &x0, &y0, &ww, &hh)) return;
    int x1 = x0 + ww, y1 = y0 + hh;
    for (int j = y0; j < y1; j++) {
        int t = h > 1 ? ((j - y) * 255) / (h - 1) : 0;
        uint32_t c = gfx_blend(top, bot, t);
        uint32_t *row = s->px + (size_t)j * s->stride;
        if (a >= 255) {
            for (int i = x0; i < x1; i++) row[i] = c;
        } else {
            for (int i = x0; i < x1; i++) row[i] = gfx_blend(row[i], c, a);
        }
    }
}

void gfx_vgradient(Surface *s, int x, int y, int w, int h, uint32_t top, uint32_t bot)
{
    gfx_vgradient_a(s, x, y, w, h, top, bot, 255);
}

/* 简易阴影：多层低透明度黑色圆角矩形 */
void gfx_shadow(Surface *s, int x, int y, int w, int h, int r, int spread)
{
    for (int i = spread; i >= 1; i--) {
        int a = 12 - i * 2;
        if (a <= 2) a = 2;
        gfx_fill_round_rect_a(s, x - i, y - i + 2, w + i * 2, h + i * 2, r + i, C_BLACK, a);
    }
}

/* ------------------------------------------------------------------ */
/* 图像                                                                */
/* ------------------------------------------------------------------ */
void gfx_blit_a(Surface *dst, int dx, int dy, const Surface *src, int a)
{
    if (!dst || !src) return;
    int x0 = dx, y0 = dy, ww = src->w, hh = src->h;
    if (!clip_rect(dst, &x0, &y0, &ww, &hh)) return;
    int x1 = x0 + ww, y1 = y0 + hh;
    for (int j = y0; j < y1; j++) {
        uint32_t *drow = dst->px + (size_t)j * dst->stride;
        const uint32_t *srow = src->px + (size_t)(j - dy) * src->stride;
        if (a >= 255) {
            for (int i = x0; i < x1; i++) drow[i] = srow[i - dx];
        } else {
            for (int i = x0; i < x1; i++) drow[i] = gfx_blend(drow[i], srow[i - dx], a);
        }
    }
}

void gfx_blit(Surface *dst, int dx, int dy, const Surface *src)
{
    gfx_blit_a(dst, dx, dy, src, 255);
}

/* 透明键贴图：跳过 GX_KEY 像素 */
void gfx_blit_keyed_a(Surface *dst, int dx, int dy, const Surface *src, int a)
{
    if (!dst || !src || a <= 0) return;
    int x0 = dx, y0 = dy, ww = src->w, hh = src->h;
    if (!clip_rect(dst, &x0, &y0, &ww, &hh)) return;
    int x1 = x0 + ww, y1 = y0 + hh;
    for (int j = y0; j < y1; j++) {
        uint32_t *drow = dst->px + (size_t)j * dst->stride;
        const uint32_t *srow = src->px + (size_t)(j - dy) * src->stride;
        for (int i = x0; i < x1; i++) {
            uint32_t v = srow[i - dx];
            if (v == GX_KEY) continue;
            drow[i] = (a >= 255) ? v : gfx_blend(drow[i], v, a);
        }
    }
}

void gfx_blit_keyed(Surface *dst, int dx, int dy, const Surface *src)
{
    gfx_blit_keyed_a(dst, dx, dy, src, 255);
}

void gfx_blit_scaled(Surface *dst, int dx, int dy, int dw, int dh,
                     const Surface *src, int sx, int sy, int sw, int sh, int a)
{
    if (!dst || !src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int x0 = dx, y0 = dy, ww = dw, hh = dh;
    if (!clip_rect(dst, &x0, &y0, &ww, &hh)) return;
    int x1 = x0 + ww, y1 = y0 + hh;

    /* 定点步进，避免每像素一次除法 */
    uint32_t stepx = (uint32_t)((double)sw * 65536.0 / dw);
    uint32_t stepy = (uint32_t)((double)sh * 65536.0 / dh);
    for (int j = y0; j < y1; j++) {
        uint32_t v = (uint32_t)((j - dy) * stepy) + ((uint32_t)sy << 16);
        int srcy = (int)(v >> 16);
        if (srcy >= src->h) srcy = src->h - 1;
        if (srcy < 0) srcy = 0;
        const uint32_t *srow = src->px + (size_t)srcy * src->stride;
        uint32_t *drow = dst->px + (size_t)j * dst->stride;
        uint32_t u = (uint32_t)((x0 - dx) * stepx) + ((uint32_t)sx << 16);
        for (int i = x0; i < x1; i++, u += stepx) {
            int srcx = (int)(u >> 16);
            if (srcx >= src->w) srcx = src->w - 1;
            if (srcx < 0) srcx = 0;
            uint32_t c = srow[srcx];
            drow[i] = (a >= 255) ? c : gfx_blend(drow[i], c, a);
        }
    }
}

void gfx_blit_cover(Surface *dst, int dx, int dy, int dw, int dh,
                    const Surface *src, int a)
{
    if (!dst || !src || dw <= 0 || dh <= 0) return;
    /* 等比缩放至铺满，再居中裁剪 */
    double s_ar = (double)src->w / src->h;
    double d_ar = (double)dw / dh;
    int sw, sh, sx, sy;
    if (s_ar > d_ar) {
        sh = src->h; sy = 0;
        sw = (int)(src->h * d_ar + 0.5);
        sx = (src->w - sw) / 2;
    } else {
        sw = src->w; sx = 0;
        sh = (int)(src->w / d_ar + 0.5);
        sy = (src->h - sh) / 2;
    }
    gfx_blit_scaled(dst, dx, dy, dw, dh, src, sx, sy, sw, sh, a);
}

/* ------------------------------------------------------------------ */
/* 离屏画布                                                            */
/* ------------------------------------------------------------------ */
Surface *gfx_surface_new(int w, int h)
{
    if (w <= 0 || h <= 0) return NULL;
    Surface *s = (Surface *)malloc(sizeof(Surface));
    if (!s) return NULL;
    s->px = (uint32_t *)calloc((size_t)w * h, 4);
    if (!s->px) { free(s); return NULL; }
    s->w = w; s->h = h; s->stride = w;
    return s;
}

Surface *gfx_surface_new_keyed(int w, int h)
{
    Surface *s = gfx_surface_new(w, h);
    if (!s) return NULL;
    for (size_t i = 0; i < (size_t)w * h; i++) s->px[i] = GX_KEY;
    return s;
}

void gfx_surface_free(Surface *s)
{
    if (!s) return;
    free(s->px);
    free(s);
}
