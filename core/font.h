/*
 * font.h — 文字渲染（stb_truetype 运行时栅格化）
 *
 * 用 TrueType 字体在运行时栅格化字形并缓存，因此**任意中文都能显示**
 * （LLM 回复、拼音候选不再受预生成图集的字形数限制）。
 *
 * 度量与原先的位图图集逐像素对齐：ascent/descent 沿用图集常数、
 * advance 用 round(hmtx × scale) —— 实测 538/538 字形 advance 完全一致，
 * 所以所有既有界面的版面不会挪位（见 design/18-cjk-text-rendering.md）。
 *
 * 字体路径：环境变量 SHIXI_TTF → 板上 /root/shixi/SimHei.ttf → 主机 tools/SimHei.ttf
 */
#ifndef SHIXI_FONT_H
#define SHIXI_FONT_H

#include "gfx.h"

typedef enum {
    FONT_SMALL = 0,   /* 15px：状态栏、说明文字 */
    FONT_BODY  = 1,   /* 19px：正文、按钮 */
    FONT_TITLE = 2,   /* 26px：标题 */
    FONT_HUGE  = 3,   /* 44px：时钟、大号数字 */
    FONT_COUNT
} FontId;

int font_init(void);                                  /* 加载字体并准备缓存，返回 0 成功 */
int font_height(FontId f);                            /* 行高 */
int font_ascent(FontId f);                            /* 基线到行顶距离 */

int text_width(FontId f, const char *utf8);           /* 单行宽度(像素) */
int text_draw(Surface *s, int x, int y, const char *utf8, FontId f, uint32_t color);
int text_draw_a(Surface *s, int x, int y, const char *utf8, FontId f, uint32_t color, int alpha);
/* y 为行顶；返回绘制后光标 x */
int text_draw_center(Surface *s, int cx, int y, const char *utf8, FontId f, uint32_t color);
int text_draw_right(Surface *s, int rx, int y, const char *utf8, FontId f, uint32_t color);
/* 超出 max_w 时截断并追加省略号 */
int text_draw_ellipsis(Surface *s, int x, int y, const char *utf8, FontId f, uint32_t color, int max_w);
/* 垂直居中于 [y, y+h] */
int text_draw_vcenter(Surface *s, int x, int y, int h, const char *utf8, FontId f, uint32_t color);
int text_draw_vcenter_center(Surface *s, int cx, int y, int h, const char *utf8, FontId f, uint32_t color);

/* ---------- 多行排版（LLM 回复、长文本） ---------- */
/*
 * 换行规则：\n 强制断行；汉字/全角字之间可任意断行；拉丁按空白与连字符断词，
 * 单词本身超宽时硬断。行距 = font_height(f) + line_gap。
 * max_lines 用尽时最后一行按 text_draw_ellipsis 的规则补 "...".
 */
typedef struct {
    const char *start;   /* 指向原文，不拥有内存 */
    int         bytes;   /* 该行字节数（不含换行符） */
    int         width;   /* 该行像素宽度 */
} TextLine;

/* 拆行；out 可为 NULL（只数行数）。返回实际行数（不超过 max_lines） */
int text_wrap_split(const char *utf8, FontId f, int max_w, TextLine *out, int max_lines);
/* 只量高度：聊天区先量后画，用来定气泡尺寸 */
int text_wrap_height(FontId f, const char *utf8, int max_w, int max_lines, int line_gap);
/* 按宽度换行绘制；y 为第一行行顶；返回绘制后下一行的 y */
int text_wrap_draw(Surface *s, int x, int y, int max_w, int max_lines,
                   const char *utf8, FontId f, uint32_t color, int line_gap);

#endif /* SHIXI_FONT_H */
