/*
 * fontprobe.c — 字体回归探针（主机程序，不参与板上构建）
 *
 * 把「新引擎」（core/font.c，stb_truetype 运行时栅格化）与「归档的位图图集」
 * （archive/font_atlas.h，-Iarchive）放在一起比：
 *
 *   1) 行高/基线     —— 必须与图集常数一致
 *   2) 逐字形 advance —— 必须与图集逐字一致（版面零位移的根据）
 *   3) 观感对照       —— 同一批文案，两种渲染各画一份
 *   4) 多行排版       —— text_wrap_* 的换行点、行距、缺字方框
 *
 * 也可以拿它评估「换一个字体会不会挪版面」：
 *   ./bin/fontprobe /home/gec/simkai.ttf
 *
 * 用法：make fontprobe && ./bin/fontprobe [TTF路径]
 * 输出：表格（stdout）+ /tmp/font_ab.ppm（转 PNG：python3 tools/fb2png.py /tmp/font_ab.ppm）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "font.h"
#include "font_atlas.h"     /* 归档的版面基准 */

/* 归档图集是 zlib 压缩的，解压函数在 third_party/stb_impl.c */
extern char *stbi_zlib_decode_malloc(const char *buffer, int len, int *outlen);

static unsigned char *atlas_px[FONT_ATLAS_COUNT];

/* ---------- 归档图集的渲染（旧引擎的等价实现，只为做对照） ---------- */
static const FontGlyph *atlas_find(const FontAtlas *a, uint32_t cp)
{
    int lo = 0, hi = a->glyph_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (a->glyphs[mid].cp == cp) return &a->glyphs[mid];
        if (a->glyphs[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static uint32_t u8next(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t c = *p;
    if (c < 0x80) { *pp += 1; return c; }
    if ((c & 0xE0) == 0xC0) { c = ((c & 0x1F) << 6) | (p[1] & 0x3F); *pp += 2; return c; }
    if ((c & 0xF0) == 0xE0) { c = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); *pp += 3; return c; }
    if ((c & 0xF8) == 0xF0) { c = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); *pp += 4; return c; }
    *pp += 1;
    return c;
}

static void atlas_draw(Surface *s, int x, int y, const char *utf8, int idx, uint32_t color)
{
    const FontAtlas *a = &g_font_atlases[idx];
    const unsigned char *px = atlas_px[idx];
    int baseline = y + a->ascent;
    int cx = x;
    int cx0, cy0, cx1, cy1;
    gfx_clip_rect(&cx0, &cy0, &cx1, &cy1);
    const char *p = utf8;
    while (*p) {
        uint32_t cp = u8next(&p);
        if (cp == '\n') break;
        const FontGlyph *g = atlas_find(a, cp);
        if (!g) { cx += a->px / 3; continue; }
        if (g->w && g->h) {
            int gx = cx + g->bx, gy = baseline + g->by;
            for (int j = 0; j < g->h; j++) {
                int dy = gy + j;
                if (dy < 0 || dy >= s->h || dy < cy0 || dy >= cy1) continue;
                uint32_t *drow = s->px + (size_t)dy * s->stride;
                for (int i = 0; i < g->w; i++) {
                    int dx = gx + i;
                    if (dx < 0 || dx >= s->w || dx < cx0 || dx >= cx1) continue;
                    int cov;
                    if (a->bpp == 4) {
                        size_t k = (size_t)(g->gy + j) * a->atlas_w + g->gx + i;
                        unsigned char b = px[k >> 1];
                        cov = ((k & 1) ? (b & 0x0F) : (b >> 4)) * 17;
                    } else {
                        cov = px[(size_t)(g->gy + j) * a->atlas_w + g->gx + i];
                    }
                    if (cov) drow[dx] = gfx_blend(drow[dx], color, cov);
                }
            }
        }
        cx += g->adv;
    }
}

/* 把一个码点写成 UTF-8 */
static int cp_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* 统计一块区域的墨量（与背景色不同的像素数） */
static long band_ink(Surface *s, int y0, int h)
{
    long n = 0;
    for (int y = y0; y < y0 + h && y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            if (s->px[(size_t)y * s->stride + x] != C_BG) n++;
    return n;
}

static const char *SAMPLE_B = "今天天气怎么样？AI 聊天";          /* 图集缺「样/怎」等 */
static const char *LONG_TEXT =
    "你好，我是石溪相机内置的助手，可以帮你查资料、写代码。"
    "今天天气怎么样？\n"
    "简体与繁體都能显示，未收录的字画空心方框：𠮷 😀";

int main(int argc, char **argv)
{
    if (argc > 1) setenv("SHIXI_TTF", argv[1], 1);
    if (font_init() != 0) { fprintf(stderr, "字体加载失败\n"); return 1; }

    const char *path = getenv("SHIXI_TTF");
    printf("字体: %s\n\n", path ? path : "（默认）");

    /* ---------- 1. 行高 / 基线 ---------- */
    printf("== 行高 / 基线（新引擎 vs 归档图集）==\n");
    int metric_bad = 0;
    for (int i = 0; i < FONT_COUNT; i++) {
        const FontAtlas *a = &g_font_atlases[i];
        int h = font_height((FontId)i), asc = font_ascent((FontId)i);
        int ok = (asc == a->ascent) && (h == a->ascent + a->descent);
        if (!ok) metric_bad++;
        printf("  %-12s 引擎 %2d+%2d=%2d | 图集 %2d+%2d=%2d  %s\n",
               a->name, asc, h - asc, h, a->ascent, a->descent, a->ascent + a->descent,
               ok ? "一致" : "**不一致**");
    }

    /* ---------- 2. 逐字形 advance ---------- */
    printf("\n== 逐字形 advance（图集已收录的 %d 个字）==\n", g_font_atlases[FONT_BODY].glyph_count);
    int total_bad = 0;
    for (int i = 0; i < FONT_COUNT; i++) {
        const FontAtlas *a = &g_font_atlases[i];
        int same = 0, diff = 0, worst = 0;
        uint32_t worst_cp = 0;
        for (int j = 0; j < a->glyph_count; j++) {
            char buf[8];
            int n = cp_utf8(a->glyphs[j].cp, buf);
            buf[n] = 0;
            int d = text_width((FontId)i, buf) - (int)a->glyphs[j].adv;
            if (d == 0) same++;
            else {
                diff++;
                if (d < 0) d = -d;
                if (d > worst) { worst = d; worst_cp = a->glyphs[j].cp; }
            }
        }
        total_bad += diff;
        printf("  %-12s 一致 %3d/%3d，偏差 %3d，最大 %2d px%s\n",
               a->name, same, a->glyph_count, diff, worst,
               diff ? "  ← 检查" : "");
        if (diff && worst_cp) printf("               最差 U+%04X\n", worst_cp);
    }

    /* ---------- 3. 换行结果 ---------- */
    printf("\n== 多行排版（max_w=420, max_lines=5, line_gap=4）==\n");
    TextLine lines[5];
    int n = text_wrap_split(LONG_TEXT, FONT_BODY, 420, lines, 5);
    for (int i = 0; i < n; i++) {
        printf("  第 %d 行 %3d 字节 %3d px: %.*s\n", i + 1, lines[i].bytes, lines[i].width,
               lines[i].bytes, lines[i].start);
    }
    printf("  text_wrap_height = %d px\n", text_wrap_height(FONT_BODY, LONG_TEXT, 420, 5, 4));

    /* ---------- 4. 观感对照图 ---------- */
    if (gfx_init() != 0) { fprintf(stderr, "gfx_init 失败（需要 -DSHIXI_HOST）\n"); return 1; }
    for (int i = 0; i < FONT_ATLAS_COUNT; i++) {
        int need = g_font_atlases[i].bpp == 4
                 ? (g_font_atlases[i].atlas_w * g_font_atlases[i].atlas_h + 1) / 2
                 : (g_font_atlases[i].atlas_w * g_font_atlases[i].atlas_h);
        int got = 0;
        atlas_px[i] = (unsigned char *)stbi_zlib_decode_malloc(
            (const char *)g_font_atlases[i].bits_z, g_font_atlases[i].bits_z_len, &got);
        if (!atlas_px[i] || got < need) { fprintf(stderr, "归档图集解压失败\n"); return 1; }
    }

    Surface *s = gfx_back();
    gfx_reset_clip();

    /* ---------- 隔离测墨：清屏后只画一种引擎，避免行带串扰 ---------- */
    printf("\n== 同一句「%s」的墨量（隔离测量）==\n", SAMPLE_B);
    for (int i = 0; i < FONT_COUNT; i++) {
        int bh = font_height((FontId)i) + 12;
        long ink_atlas, ink_engine;
        gfx_fill_rect(s, 0, 0, s->w, s->h, C_BG);
        atlas_draw(s, 8, 10, SAMPLE_B, i, C_ACCENT);
        ink_atlas = band_ink(s, 0, bh);
        gfx_fill_rect(s, 0, 0, s->w, s->h, C_BG);
        text_draw(s, 8, 10, SAMPLE_B, (FontId)i, C_ACCENT);
        ink_engine = band_ink(s, 0, bh);
        printf("  %-12s 图集 %6ld   新引擎 %6ld   %s\n",
               g_font_atlases[i].name, ink_atlas, ink_engine,
               ink_atlas == 0 ? "（图集在这个字号完全没有这些字）" : "");
    }

    gfx_clear(s, C_BG);
    int y = 4;
    text_draw(s, 8, y, "上=归档图集（旧）  下=新引擎（stb_truetype）：缺字处旧引擎留空、新引擎照常渲染",
              FONT_SMALL, C_TEXT_MUTED);
    y += font_height(FONT_SMALL) + 2;
    for (int i = 0; i < FONT_COUNT; i++) {
        char cap[64];
        snprintf(cap, sizeof(cap), "%s", g_font_atlases[i].name);
        text_draw(s, 8, y, cap, FONT_SMALL, C_TEXT_DIM);
        y += font_height(FONT_SMALL);
        atlas_draw(s, 8, y, SAMPLE_B, i, C_ACCENT);
        y += font_height((FontId)i);
        text_draw(s, 8, y, SAMPLE_B, (FontId)i, C_ACCENT);
        y += font_height((FontId)i) + 1;
    }
    y += 4;
    text_draw(s, 8, y, "多行 + 任意中文 + 缺字方框（新引擎，f_body）：", FONT_SMALL, C_TEXT_MUTED);
    y += font_height(FONT_SMALL) + 2;
    text_wrap_draw(s, 8, y, 780, 6, LONG_TEXT, FONT_BODY, C_TEXT, 4);

    if (gfx_save_ppm("/tmp/font_ab.ppm") != 0) fprintf(stderr, "保存失败\n");
    else printf("\n对照图：/tmp/font_ab.ppm\n");

    int fail = metric_bad + total_bad;
    printf("\n%s（度量不一致项 %d）\n", fail ? "**有偏差，需检查**" : "度量门禁通过", fail);
    gfx_shutdown();
    return fail ? 2 : 0;
}
