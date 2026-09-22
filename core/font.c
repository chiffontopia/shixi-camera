/*
 * font.c — stb_truetype 运行时栅格化（取代原先的预生成位图图集）
 *
 * 三条设计约束，动机见 design/18-cjk-text-rendering.md：
 *
 * 1) 版面零位移：ascent/descent 直接沿用原图集常数，advance 用
 *    round(hmtx × scale)。实测图集里已收录的 538 个字形 advance 与 stb
 *    逐字完全一致，所以 124 处绘制调用点的坐标不用动。
 * 2) 线程安全：UI 线程与相机采集线程都会画字（capture_thread →
 *    testpattern_render），因此所有公开入口持有一把互斥锁；内部一律走
 *    不带锁的 *_span / *_locked 形式，避免递归加锁。
 * 3) 未收录码点画空心方框，不静默吞掉（emoji 会显示成方框）。
 */
#include "font.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"

/* ---------- 各字号的像素大小与度量常数 ---------- */
/* ascent/descent 取自原图集（13/3、17/3、23/4、38/7），保持行高与基线一致 */
typedef struct { int px, ascent, descent; } FaceMetrics;
static const FaceMetrics FM[FONT_COUNT] = {
    { 15, 13, 3 },   /* FONT_SMALL */
    { 19, 17, 3 },   /* FONT_BODY  */
    { 26, 23, 4 },   /* FONT_TITLE */
    { 44, 38, 7 },   /* FONT_HUGE  */
};

#define CACHE_SLOTS 1024          /* 每字号的字形缓存槽位 */

typedef struct {
    uint32_t cp;                  /* 0 = 空槽 */
    int16_t  w, h, bx, by;        /* 位图尺寸与相对基线原点偏移 */
    int16_t  adv;                 /* 步进宽度 */
    uint8_t  miss;                /* 字体没有这个码点 */
    uint8_t  have_bm;
    uint32_t used;                /* LRU 时间戳 */
    unsigned char *bm;            /* w*h 的 8bit 覆盖率 */
} Glyph;

typedef struct {
    Glyph    slot[CACHE_SLOTS];
    uint32_t tick;
    float    scale;
} Face;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char  *g_data;
static stbtt_fontinfo  g_ttf;
static Face            g_faces[FONT_COUNT];
static int             g_ready;

#define FONT_LOCK()   pthread_mutex_lock(&g_lock)
#define FONT_UNLOCK() pthread_mutex_unlock(&g_lock)

/* ---------- UTF-8（容错：截断的序列按单字节处理，与旧实现一致） ---------- */
static uint32_t utf8_next(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t c = *p;
    if (c < 0x80) {
        *pp = (const char *)(p + 1);
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        if (!p[1]) { *pp = (const char *)(p + 1); return c; }
        c = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        *pp = (const char *)(p + 2);
        return c;
    } else if ((c & 0xF0) == 0xE0) {
        if (!p[1] || !p[2]) { *pp = (const char *)(p + 1); return c; }
        c = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *pp = (const char *)(p + 3);
        return c;
    } else if ((c & 0xF8) == 0xF0) {
        if (!p[1] || !p[2] || !p[3]) { *pp = (const char *)(p + 1); return c; }
        c = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        *pp = (const char *)(p + 4);
        return c;
    }
    *pp = (const char *)(p + 1);
    return c;
}

/* ---------- 字体加载 ---------- */
static const char *font_path(void)
{
    const char *env = getenv("SHIXI_TTF");
    if (env && *env) return env;
#ifdef SHIXI_HOST
    return "tools/SimHei.ttf";        /* 主机模拟器：从仓库根目录运行 */
#else
    return "/root/shixi/SimHei.ttf";  /* 板上：部署脚本会推这一份 */
#endif
}

static int font_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0 || n <= 0) { fclose(f); return -1; }
    g_data = (unsigned char *)malloc((size_t)n);
    if (!g_data || fread(g_data, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -1; }
    fclose(f);
    int off = stbtt_GetFontOffsetForIndex(g_data, 0);   /* .ttc 集合取第 0 份 */
    if (off < 0 || !stbtt_InitFont(&g_ttf, g_data, off)) return -1;
    return 0;
}

int font_init(void)
{
    if (g_ready) return 0;
    const char *path = font_path();
    if (font_load(path) != 0) {
        fprintf(stderr, "font: 加载字体失败 %s（可用 SHIXI_TTF 指定其它字体）\n", path);
        free(g_data);
        g_data = NULL;
        return -1;
    }
    for (int i = 0; i < FONT_COUNT; i++) {
        memset(g_faces[i].slot, 0, sizeof(g_faces[i].slot));
        g_faces[i].tick = 0;
        g_faces[i].scale = stbtt_ScaleForPixelHeight(&g_ttf, (float)FM[i].px);
    }
    g_ready = 1;
    return 0;
}

int font_height(FontId f)
{
    if (f < 0 || f >= FONT_COUNT) f = FONT_BODY;
    return FM[f].ascent + FM[f].descent;
}

int font_ascent(FontId f)
{
    if (f < 0 || f >= FONT_COUNT) f = FONT_BODY;
    return FM[f].ascent;
}

/* ---------- 字形缓存 ---------- */
static void glyph_reset(Glyph *g)
{
    if (g->bm) free(g->bm);
    memset(g, 0, sizeof(*g));
}

static void glyph_metrics(FontId f, Glyph *g, uint32_t cp)
{
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&g_ttf, cp, &adv, &lsb);
    int gi = stbtt_FindGlyphIndex(&g_ttf, cp);
    if (!gi) {
        /* 未收录：占一个全角宽度，绘制时画空心方框 */
        g->miss = 1;
        g->adv = (int16_t)(FM[f].ascent + FM[f].descent);
        return;
    }
    g->adv = (int16_t)((float)adv * g_faces[f].scale + 0.5f);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&g_ttf, gi, g_faces[f].scale, g_faces[f].scale, &x0, &y0, &x1, &y1);
    g->bx = (int16_t)x0;
    g->by = (int16_t)y0;
    g->w = (int16_t)(x1 - x0);
    g->h = (int16_t)(y1 - y0);
    if (g->w <= 0 || g->h <= 0) { g->w = 0; g->h = 0; }   /* 空格之类：只有宽度 */
}

static void glyph_bitmap(FontId f, Glyph *g)
{
    if (g->have_bm || g->miss || g->w <= 0 || g->h <= 0) return;
    int gi = stbtt_FindGlyphIndex(&g_ttf, g->cp);
    if (!gi) { g->miss = 1; g->have_bm = 1; return; }
    size_t bytes = (size_t)g->w * (size_t)g->h;
    unsigned char *bm = (unsigned char *)malloc(bytes);
    if (!bm) return;                       /* 内存不足：这帧不画，下帧再试 */
    stbtt_MakeGlyphBitmap(&g_ttf, bm, g->w, g->h, g->w, g_faces[f].scale, g_faces[f].scale, gi);
    g->bm = bm;
    g->have_bm = 1;
}

/* 取字形（want_bitmap=0 只取度量，不栅格化，供测宽/换行用）。锁内调用 */
static Glyph *glyph_get(FontId f, uint32_t cp, int want_bitmap)
{
    Face *fc = &g_faces[f];
    uint32_t h = (cp * 2654435761u) & (CACHE_SLOTS - 1);
    int empty = -1, victim = -1;
    uint32_t oldest = 0xFFFFFFFFu;

    for (int probe = 0; probe < CACHE_SLOTS; probe++) {
        int idx = (int)((h + (uint32_t)probe) & (CACHE_SLOTS - 1));
        Glyph *g = &fc->slot[idx];
        if (g->cp == cp) {
            g->used = ++fc->tick;
            if (want_bitmap) glyph_bitmap(f, g);
            return g;
        }
        if (g->cp == 0) { empty = idx; break; }
        if (g->used < oldest) { oldest = g->used; victim = idx; }
    }
    /* 槽位满了就淘汰本条探测链里最久未用的（必须在本链内换，否则新键之后找不到） */
    int idx = (empty >= 0) ? empty : victim;
    Glyph *g = &fc->slot[idx];
    glyph_reset(g);
    g->cp = cp;
    g->used = ++fc->tick;
    glyph_metrics(f, g, cp);
    if (want_bitmap) glyph_bitmap(f, g);
    return g;
}

/* ---------- 绘制 ---------- */
static int draw_span(Surface *s, int x, int y, const char *p, const char *end,
                     FontId f, uint32_t color, int alpha);

/* 未收录码点：在字身框里画一个 1px 空心方框 */
static void draw_missing(Surface *s, int x, int baseline, FontId f, int adv,
                         uint32_t color, int alpha)
{
    int lh = FM[f].ascent + FM[f].descent;
    int side = lh / 2;
    if (side < 5) side = 5;
    if (side > adv - 2) side = adv - 2;
    if (side < 3) return;
    int top = baseline - FM[f].ascent + (lh - side) / 2;
    int left = x + (adv - side) / 2;
    gfx_fill_rect_a(s, left, top, side, 1, color, alpha);
    gfx_fill_rect_a(s, left, top + side - 1, side, 1, color, alpha);
    gfx_fill_rect_a(s, left, top, 1, side, color, alpha);
    gfx_fill_rect_a(s, left + side - 1, top, 1, side, color, alpha);
}

static int draw_span(Surface *s, int x, int y, const char *p, const char *end,
                     FontId f, uint32_t color, int alpha)
{
    const int baseline = y + FM[f].ascent;
    int cx = x;
    int cx0, cy0, cx1, cy1;
    gfx_clip_rect(&cx0, &cy0, &cx1, &cy1);

    while (p < end) {
        uint32_t cp = utf8_next(&p);
        if (cp == '\n') break;
        if (cp == '\r') continue;
        Glyph *g = glyph_get(f, cp, 1);
        if (!g) continue;
        if (g->miss) {
            draw_missing(s, cx, baseline, f, g->adv, color, alpha);
            cx += g->adv;
            continue;
        }
        if (g->have_bm && g->w && g->h) {
            int gx = cx + g->bx;
            int gy = baseline + g->by;
            for (int j = 0; j < g->h; j++) {
                int dy = gy + j;
                if (dy < 0 || dy >= s->h || dy < cy0 || dy >= cy1) continue;
                const unsigned char *srow = g->bm + (size_t)j * g->w;
                uint32_t *drow = s->px + (size_t)dy * s->stride;
                for (int i = 0; i < g->w; i++) {
                    int cov = srow[i];
                    if (!cov) continue;
                    int dx = gx + i;
                    if (dx < 0 || dx >= s->w || dx < cx0 || dx >= cx1) continue;
                    int aa = (alpha >= 255) ? cov : cov * alpha / 255;
                    if (aa <= 0) continue;
                    drow[dx] = gfx_blend(drow[dx], color, aa);
                }
            }
        }
        cx += g->adv;
    }
    return cx;
}

static int width_span(FontId f, const char *p, const char *end)
{
    int w = 0;
    while (p < end) {
        uint32_t cp = utf8_next(&p);
        if (cp == '\n') break;
        if (cp == '\r') continue;
        Glyph *g = glyph_get(f, cp, 0);
        if (g) w += g->adv;
    }
    return w;
}

static int ellipsis_span(Surface *s, int x, int y, const char *utf8, FontId f,
                         uint32_t color, int max_w)
{
    if (!utf8) return x;
    const char *end = utf8 + strlen(utf8);
    if (width_span(f, utf8, end) <= max_w)
        return draw_span(s, x, y, utf8, end, f, color, 255);

    static const char dots[] = "...";
    int budget = max_w - width_span(f, dots, dots + 3);
    if (budget < 0) budget = 0;

    char buf[256];
    int bi = 0, w = 0;
    const char *p = utf8;
    while (p < end) {
        const char *cs = p;
        uint32_t cp = utf8_next(&p);
        if (cp == '\n') break;
        Glyph *g = glyph_get(f, cp, 0);
        int adv = g ? g->adv : 0;
        if (w + adv > budget) break;
        int len = (int)(p - cs);
        if (bi + len + 4 >= (int)sizeof(buf)) break;
        memcpy(buf + bi, cs, len);
        bi += len;
        w += adv;
    }
    memcpy(buf + bi, dots, 4);
    return draw_span(s, x, y, buf, buf + bi + 3, f, color, 255);
}

/* ---------- 换行 ---------- */
/* 可在「该字符之前」断行：汉字/全角两侧、空白或连字符之后 */
static int is_cjk(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x11FF) ||      /* 谚文字母 */
           (cp >= 0x2E80 && cp <= 0xA4CF) ||      /* 部首、汉字、假名、注音 */
           (cp >= 0xAC00 && cp <= 0xD7A3) ||      /* 谚文音节 */
           (cp >= 0xF900 && cp <= 0xFAFF) ||      /* 兼容汉字 */
           (cp >= 0xFF00 && cp <= 0xFF60) ||      /* 全角形式 */
           (cp >= 0x20000);
}

/* 中文禁则：这些标点不许落在行首 / 行尾（否则「、」开头、「（」结尾很刺眼） */
static int no_line_start(uint32_t cp)
{
    switch (cp) {
    case ',': case '.': case '!': case '?': case ':': case ';': case ')': case ']': case '}':
    case 0x3001: case 0x3002:                      /* 、。 */
    case 0xFF0C: case 0xFF01: case 0xFF1F: case 0xFF1A: case 0xFF1B: case 0xFF1E:
    case 0xFF09: case 0xFF3D: case 0xFF5D:         /* ，！？：；）］｝ */
    case 0x300D: case 0x300F: case 0x3011: case 0x3015: case 0x3017: case 0x3019: case 0x301B:
    case 0x2026: case 0x2014: case 0x00B7:         /* … — · */
        return 1;
    default:
        return 0;
    }
}

static int no_line_end(uint32_t cp)
{
    switch (cp) {
    case '(': case '[': case '{':
    case 0xFF08: case 0x3014:                      /* （〔 */
    case 0x3008: case 0x300A: case 0x300C: case 0x300E: case 0x3010: case 0x3016:
        return 1;
    default:
        return 0;
    }
}

static int breakable(uint32_t cp, uint32_t prev)
{
    if (no_line_start(cp) || no_line_end(prev)) return 0;
    return is_cjk(cp) || is_cjk(prev) || prev == ' ' || prev == '-' || prev == 0x3000;
}

/* 一行能放多少字节；*w_out 回报该行宽度。锁内调用 */
static int line_fit(FontId f, const char *p, const char *end, int max_w, int *w_out)
{
    const char *q = p, *brk = NULL;
    int w = 0, brk_w = 0;
    uint32_t prev = 0;

    while (q < end) {
        const char *cs = q;
        uint32_t cp = utf8_next(&q);
        if (cp == '\n') { q = cs; break; }     /* 换行符不算进本行 */
        if (cp == '\r') continue;
        Glyph *g = glyph_get(f, cp, 0);
        int adv = g ? g->adv : 0;
        if (cs > p && breakable(cp, prev)) { brk = cs; brk_w = w; }
        if (w + adv > max_w && cs > p) {
            if (brk) { *w_out = brk_w; return (int)(brk - p); }
            *w_out = w;
            return (int)(cs - p);              /* 单个词就超宽：硬断 */
        }
        prev = cp;
        w += adv;
    }
    *w_out = w;
    return (int)(q - p);
}

/* 取下一行的起点（跳过刚消费掉的换行符/行首空白）。锁内调用 */
static const char *next_line(const char *p, const char *end, int broke_on_width)
{
    if (p < end && *p == '\r') p++;
    if (p < end && *p == '\n') return p + 1;
    if (broke_on_width) {
        while (p < end && *p == ' ') p++;      /* 宽度断行时丢掉续行的前导空格 */
    }
    return p;
}

int text_wrap_split(const char *utf8, FontId f, int max_w, TextLine *out, int max_lines)
{
    if (!utf8 || max_w <= 0 || max_lines <= 0) return 0;
    if (f < 0 || f >= FONT_COUNT) f = FONT_BODY;

    FONT_LOCK();
    int n = 0;
    if (g_ready || font_init() == 0) {
        const char *end = utf8 + strlen(utf8);
        const char *p = utf8;
        while (p < end && n < max_lines) {
            int w = 0;
            int bytes = line_fit(f, p, end, max_w, &w);
            if (out) {
                out[n].start = p;
                out[n].bytes = bytes;
                out[n].width = w;
            }
            n++;
            const char *np = p + bytes;
            int by_width = (np < end && *np != '\n' && *np != '\r');
            p = next_line(np, end, by_width);
        }
    }
    FONT_UNLOCK();
    return n;
}

int text_wrap_height(FontId f, const char *utf8, int max_w, int max_lines, int line_gap)
{
    int lines = text_wrap_split(utf8, f, max_w, NULL, max_lines);
    if (lines <= 0) return 0;
    return lines * font_height(f) + (lines - 1) * line_gap;
}

int text_wrap_draw(Surface *s, int x, int y, int max_w, int max_lines,
                   const char *utf8, FontId f, uint32_t color, int line_gap)
{
    if (!s || !utf8 || max_w <= 0 || max_lines <= 0) return y;
    if (f < 0 || f >= FONT_COUNT) f = FONT_BODY;

    FONT_LOCK();
    int cy = y;
    if (g_ready || font_init() == 0) {
        const char *end = utf8 + strlen(utf8);
        const char *p = utf8;
        int n = 0;
        while (p < end && n < max_lines) {
            int w = 0;
            const char *ls = p;
            int bytes = line_fit(f, p, end, max_w, &w);
            const char *np = ls + bytes;
            int by_width = (np < end && *np != '\n' && *np != '\r');
            const char *after = next_line(np, end, by_width);
            int more = (after < end && *after != 0);

            if (more && n == max_lines - 1) {
                /* 最后一行还有内容：补省略号 */
                char buf[256];
                char *tmp = buf;
                if (bytes + 1 > (int)sizeof(buf)) tmp = (char *)malloc((size_t)bytes + 1);
                if (tmp) {
                    memcpy(tmp, ls, (size_t)bytes);
                    tmp[bytes] = 0;
                    ellipsis_span(s, x, cy, tmp, f, color, max_w);
                    if (tmp != buf) free(tmp);
                }
            } else {
                draw_span(s, x, cy, ls, ls + bytes, f, color, 255);
            }
            p = after;
            n++;
            cy += font_height(f) + line_gap;
        }
    }
    FONT_UNLOCK();
    return cy;
}

/* ---------- 公开 API：统一在这里加锁，内部函数不再加锁 ---------- */
#define WRAP_READY(no_ready_return)                       \
    do {                                                  \
        FONT_LOCK();                                      \
        if (!g_ready && font_init() != 0) { FONT_UNLOCK(); return no_ready_return; } \
    } while (0)

int text_width(FontId f, const char *utf8)
{
    if (!utf8) return 0;
    if (f < 0 || f >= FONT_COUNT) f = FONT_BODY;
    WRAP_READY(0);
    int w = width_span(f, utf8, utf8 + strlen(utf8));
    FONT_UNLOCK();
    return w;
}

int text_draw_a(Surface *s, int x, int y, const char *utf8, FontId f, uint32_t color, int alpha)
{
    if (!s || !utf8 || alpha <= 0) return x;
    if (f < 0 || f >= FONT_COUNT) f = FONT_BODY;
    WRAP_READY(x);
    int ret = draw_span(s, x, y, utf8, utf8 + strlen(utf8), f, color, alpha);
    FONT_UNLOCK();
    return ret;
}

int text_draw(Surface *s, int x, int y, const char *utf8, FontId f, uint32_t color)
{
    return text_draw_a(s, x, y, utf8, f, color, 255);
}

int text_draw_center(Surface *s, int cx, int y, const char *utf8, FontId f, uint32_t color)
{
    if (!s || !utf8) return cx;
    if (f < 0 || f >= FONT_COUNT) f = FONT_BODY;
    WRAP_READY(cx);
    const char *end = utf8 + strlen(utf8);
    int w = width_span(f, utf8, end);
    int ret = draw_span(s, cx - w / 2, y, utf8, end, f, color, 255);
    FONT_UNLOCK();
    return ret;
}

int text_draw_right(Surface *s, int rx, int y, const char *utf8, FontId f, uint32_t color)
{
    if (!s || !utf8) return rx;
    if (f < 0 || f >= FONT_COUNT) f = FONT_BODY;
    WRAP_READY(rx);
    const char *end = utf8 + strlen(utf8);
    int w = width_span(f, utf8, end);
    int ret = draw_span(s, rx - w, y, utf8, end, f, color, 255);
    FONT_UNLOCK();
    return ret;
}

int text_draw_vcenter(Surface *s, int x, int y, int h, const char *utf8, FontId f, uint32_t color)
{
    if (f < 0 || f >= FONT_COUNT) f = FONT_BODY;
    return text_draw(s, x, y + (h - font_height(f)) / 2, utf8, f, color);
}

int text_draw_vcenter_center(Surface *s, int cx, int y, int h, const char *utf8, FontId f, uint32_t color)
{
    if (f < 0 || f >= FONT_COUNT) f = FONT_BODY;
    return text_draw_center(s, cx, y + (h - font_height(f)) / 2, utf8, f, color);
}

int text_draw_ellipsis(Surface *s, int x, int y, const char *utf8, FontId f, uint32_t color, int max_w)
{
    if (!s || !utf8) return x;
    if (f < 0 || f >= FONT_COUNT) f = FONT_BODY;
    WRAP_READY(x);
    int ret = ellipsis_span(s, x, y, utf8, f, color, max_w);
    FONT_UNLOCK();
    return ret;
}
