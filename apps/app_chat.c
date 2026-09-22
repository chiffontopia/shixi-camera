/*
 * app_chat.c — AI 聊天（接真实模型）
 *
 * 说明：界面（气泡、输入框、屏幕键盘、拼音输入法）在这里，**传输在 core/ai.c**：
 *       那边 fork/exec 板上的静态 curl 打宿主机的 OpenAI 兼容接口，逐帧轮询结果。
 *       本文件只做三件事：把状态画出来、把结果变成气泡、把重试/取消接到按钮上。
 *       没配 endpoint 时退回本地关键词回复（chat_reply），并在界面上一眼标明。
 *
 * 三种气泡（ChatMsg.mine）：0 = 助手（含真模型回复），1 = 我发的，2 = 系统/错误。
 * 只有 0/1 会进远端上下文（错误气泡不发给模型）。
 *
 * 设计见 .scratch/board-desktop/design/11-ai-assistant-transport.md。
 */
#include "apps.h"
#include "ai.h"
#include "pinyin.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define TB_H 58
#define INPUT_H 56
#define BUBBLE_PAD 14
#define MAX_MSG 60
#define MAX_LINE 24
#define INPUT_MAX 180

/* 气泡身份。0/1 会被发进远端上下文；2 是系统报错，不发。 */
#define MSG_AI 0
#define MSG_ME 1
#define MSG_ERR 2

typedef struct
{
  int mine;        /* MSG_AI / MSG_ME / MSG_ERR */
  char text[1024]; /* 真模型回复可能几百字节（原先 256 会截断） */
  int t_sec;       /* 收到消息时的秒数（用于显示时间） */
} ChatMsg;

static ChatMsg msgs[MAX_MSG];
static int nmsg = 0;
static char draft[INPUT_MAX + 1];
static int kb_open = 0;
static int kb_shift = 0;
static int typing_until = 0;  /* 显示"正在输入"的截止时间 */
static int pending_reply = 0; /* 待回复标志 */
static uint64_t reply_at = 0;
static float scroll = 0; /* 消息列表滚动量（像素） */
static int dragging = 0;
static int drag_y0 = 0;
static float drag_scroll0 = 0;
static float content_h = 0;
static int waiting = 0;               /* 真模型请求进行中 */
static int show_retry = 0;            /* 上次失败，顶栏显示「重试」 */
static char last_user[INPUT_MAX + 1]; /* 上一条用户消息（重试要原样再发） */

/* ---------- 拼音输入法 ---------- */
/* 设计：拼音**不上屏**。打出来的字母只进 py[]，选中的字才进 draft[]，
 *       所以发送出去的内容永远只有确定下来的字，不会夹生拼音。 */
#define PY_MAX 24
#define CAND_MAX 12 /* 候选条一次最多显示几个；放不下不翻页，继续打字收窄 */

static char py[PY_MAX + 1];    /* 待提交拼音 */
static int cn_mode = 1;        /* 1 = 中文（拼音），0 = 英文 */
static const char *cand_chars; /* 当前音节的候选字串（NULL = 还没成音节） */

static int utf8_len(unsigned char c)
{
  if (c >= 0xF0)
    return 4;
  if (c >= 0xE0)
    return 3;
  if (c >= 0xC0)
    return 2;
  return 1;
}

static void draft_append(const char *s)
{
  int len = (int)strlen(draft);
  while (*s)
  {
    int clen = utf8_len((unsigned char)*s);
    if (len + clen > INPUT_MAX)
      break;
    memcpy(draft + len, s, (size_t)clen);
    len += clen;
    s += clen;
  }
  draft[len] = 0;
}

/* 退格删掉一个完整字符 —— draft 里现在会有中文，只删一个字节会把它拆碎 */
static void draft_backspace(void)
{
  int len = (int)strlen(draft);
  if (len == 0)
    return;
  int i = len - 1;
  while (i > 0 && ((unsigned char)draft[i] & 0xC0) == 0x80)
    i--;
  draft[i] = 0;
}

/* 太长时只显示尾部（按字符截，避免截到 UTF-8 中间变乱码） */
static void show_tail(char *show, int max_w)
{
  const char *p = show;
  while (*p && text_width(FONT_BODY, p) > max_w)
    p += utf8_len((unsigned char)*p);
  if (p != show)
    memmove(show, p, strlen(p) + 1);
}

static void py_refresh(void)
{
  int len = pinyin_first_syllable(py);
  if (len <= 0)
  {
    cand_chars = NULL;
    return;
  }
  char buf[8];
  memcpy(buf, py, (size_t)len);
  buf[len] = 0;
  cand_chars = pinyin_lookup(buf);
}

static void py_clear(void)
{
  py[0] = 0;
  cand_chars = NULL;
}

static void py_key(char ch)
{
  int n;
  if (!cn_mode)
    return;
  n = (int)strlen(py);
  if (n >= PY_MAX)
    return;
  py[n] = ch;
  py[n + 1] = 0;
  if (!pinyin_acceptable(py))
  {
    py[n] = 0;
    return;
  } /* 不可能是音节：忽略这次按键 */
  py_refresh();
}

/* 提交一个候选（one = 单个汉字的 UTF-8），并吃掉已消费的那段拼音 */
static void py_commit(const char *one)
{
  int len;
  draft_append(one);
  len = pinyin_first_syllable(py);
  if (len > 0)
    memmove(py, py + len, strlen(py + len) + 1);
  py_refresh();
}

/* 气泡高度：用 font.c 的多行排版（工单 18 的引擎，含中文禁则）。
 * 老版本这里有一份自带的 wrap_text()，把每行拷进 char[256] —— 既重复又有行长上限，
 * 现在只留一份引擎。 */
static int bubble_height(const ChatMsg *m, int *out_lines)
{
  int n = text_wrap_split(m->text, FONT_BODY, 400, NULL, MAX_LINE);
  if (out_lines)
    *out_lines = n;
  return n * font_height(FONT_BODY) + BUBBLE_PAD * 2;
}

static void add_msg(int mine, const char *text)
{
  if (nmsg >= MAX_MSG)
  {
    memmove(&msgs[0], &msgs[1], sizeof(ChatMsg) * (MAX_MSG - 1));
    nmsg = MAX_MSG - 1;
  }
  ChatMsg *m = &msgs[nmsg++];
  m->mine = mine;
  snprintf(m->text, sizeof(m->text), "%s", text);
  time_t t = time(NULL);
  struct tm lt;
  localtime_r(&t, &lt);
  m->t_sec = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
  /* 新消息自动滚到底部 */
  scroll = 1e9f;
}

/* 本地预设回复（关键词匹配） */
static const char *chat_reply(const char *q)
{
  static int fallback = 0;
  struct
  {
    const char *key;
    const char *ans;
  } table[] = {
      {"你好", "你好！我是这台开发板的 AI 助手小石。\n现在是演示版，回复来自本地预设文案。"},
      {"hi", "Hi！有什么可以帮你的吗？（演示版）"},
      {"hello", "Hello！很高兴见到你。"},
      {"拍照", "点桌面上的「拍照」就能取景，中间的白色圆键拍照，左下角可以直接进图库。"},
      {"照片", "照片都存在 /root/shixi/photo 目录，图库里可以左右滑动浏览，也能删除。"},
      {"录像", "「录像」里按红色圆键开始录，再按一次停止，会保存成 MJPEG 的 AVI 文件。"},
      {"视频", "录好的视频能在图库里直接播放，也能拷到电脑上用播放器打开。"},
      {"删除", "在图库的查看页，点右上角的垃圾桶图标就能删除，会有二次确认。"},
      {"时间", NULL},
      {"几点", NULL},
      {"名字", "我叫小石，运行在 GEC6818 开发板上，是一只界面演示用的 AI。"},
      {"是谁", "我是小石，这个项目的 AI 聊天界面演示。"},
      {"谢谢", "不客气！随时叫我。"},
      {"再见", "再见，期待下次见面！"},
      {"功能", "这台设备有四个应用：拍照、录像、图库、AI 聊天。都可以直接用触摸屏操作。"},
      {"能做什么", "我能陪聊，也能告诉你设备怎么用。真正的对话能力需要接入云端模型。"},
      {"天气", "我还没联网，查不了天气。（演示版）"},
      {"开发板", "这块是 GEC6818，8 核 Cortex-A53，7 寸 800x480 触摸屏，跑 Linux 3.4。"},
      {"触摸", "屏幕是电容触摸，坐标会自动校准，直接点就行。"},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
  {
    if (strstr(q, table[i].key))
    {
      if (table[i].ans)
        return table[i].ans;
      static char tbuf[96];
      time_t t = time(NULL);
      struct tm lt;
      localtime_r(&t, &lt);
      snprintf(tbuf, sizeof(tbuf), "现在是 %02d:%02d:%02d（开发板本地时间）。",
               lt.tm_hour, lt.tm_min, lt.tm_sec);
      return tbuf;
    }
  }
  static const char *fb[] = {
      "我还在学习中，这个问题暂时答不上来。（演示版，回复是本地预设的）",
      "这个问题有点难，等我接上大模型再回答你吧。",
      "收到！不过我现在只会几句预设台词，试试问「怎么拍照」？",
  };
  return fb[fallback++ % 3];
}

void app_chat_enter(void)
{
  ui_toast_set_bottom(96);
  ai_init(); /* 每次进来重读配置：改完 relay_* / ai.conf 即生效 */
  if (nmsg == 0)
  {
    if (ai_configured())
      add_msg(MSG_AI, "你好！我是小石。\n"
                      "随便聊点什么吧，也可以问「怎么拍照」。");
    else
      add_msg(MSG_AI, "你好！我是 AI 助手小石。\n"
                      "现在没配置模型地址，回复是本地预设文案；把 endpoint 写进 "
                      "relay_url 就能接上真模型。");
  }
  kb_open = 0;
  draft[0] = 0;
  py_clear();
  waiting = 0;
  show_retry = 0;
  scroll = 1e9f;
}

void app_chat_leave(void)
{
  kb_open = 0;
  py_clear(); /* 未提交的拼音不带到下次进来 */
  /* 请求只有一秒级，离开就取消：不占网络、也不留孤儿 curl；
   * ai_deinit() 顺带删掉那份带密钥的 curl 配置（不留 /tmp）。 */
  ai_cancel();
  waiting = 0;
  ai_deinit();
}

/* ---------------- 绘制 ---------------- */
static void draw_bubble(Surface *s, const ChatMsg *m, int x, int y, int w, int h)
{
  int right = (m->mine == MSG_ME); /* 错误气泡走左侧（和助手同侧） */
  uint32_t bg = right ? C_ACCENT
                      : (m->mine == MSG_ERR ? RGB(0x6e, 0x1c, 0x22) : C_SURFACE2);
  uint32_t fg = right ? RGB(0x08, 0x2a, 0x20)
                      : (m->mine == MSG_ERR ? RGB(0xff, 0xd2, 0xd2) : C_TEXT);
  gfx_fill_round_rect_a(s, x, y + 2, w, h, 16, C_BLACK, 60);
  gfx_fill_round_rect(s, x, y, w, h, 16, bg);
  /* 小尾巴 */
  if (right)
    gfx_fill_triangle(s, x + w - 6, y + 14, x + w + 8, y + 20, x + w - 6, y + 26, bg);
  else
    gfx_fill_triangle(s, x + 6, y + 14, x - 8, y + 20, x + 6, y + 26, bg);

  text_wrap_draw(s, x + BUBBLE_PAD, y + BUBBLE_PAD, w - BUBBLE_PAD * 2, MAX_LINE,
                 m->text, FONT_BODY, fg, 0);
}

static void draw_messages(Surface *s, int top, int bottom)
{
  /* 计算内容总高：与下面绘制时的推进方式保持一致，避免滚到底还留空 */
  int y = 10;
  for (int i = 0; i < nmsg; i++)
  {
    int lines = 0;
    int h = bubble_height(&msgs[i], &lines) + 18;
    y += h;
  }
  int typing_on = (waiting || typing_until > (int)now_ms() || pending_reply) ? 1 : 0;
  if (typing_on)
    y += 62;                   /* "正在输入"气泡：44 + 18 */
  content_h = (float)(y + 10); /* 末尾留 10px 让最新消息完整可见 */

  int view_h = bottom - top;
  float max_scroll = content_h - view_h;
  if (max_scroll < 0)
    max_scroll = 0;
  if (scroll > max_scroll)
    scroll = max_scroll;
  if (scroll < 0)
    scroll = 0;

  gfx_set_clip(0, top, SCREEN_W, view_h);
  int cy = top - (int)scroll + 10;
  int lh = font_height(FONT_BODY);

  for (int i = 0; i < nmsg; i++)
  {
    static TextLine lines[MAX_LINE];
    int nlines = text_wrap_split(msgs[i].text, FONT_BODY, 400, lines, MAX_LINE);
    int tw = 0;
    for (int k = 0; k < nlines; k++)
      if (lines[k].width > tw)
        tw = lines[k].width;
    int bw = tw + BUBBLE_PAD * 2;
    int bh = nlines * lh + BUBBLE_PAD * 2;
    int bx = (msgs[i].mine == MSG_ME) ? (SCREEN_W - 24 - bw - 10) : 74;
    /* 头像 */
    if (msgs[i].mine == MSG_AI)
    {
      gfx_fill_circle_a(s, 44, cy + 22, 18, RGB(0x6a, 0x45, 0xd8), 255);
      text_draw_vcenter_center(s, 44, cy + 4, 36, "AI", FONT_SMALL, C_WHITE);
    }
    else if (msgs[i].mine == MSG_ME)
    {
      gfx_fill_circle_a(s, SCREEN_W - 44, cy + 22, 18, C_ACCENT_DK, 255);
      icon_draw_cached(s, IC_HOME, SCREEN_W - 44, cy + 22, 18, C_WHITE);
    }
    else
    {
      gfx_fill_circle_a(s, 44, cy + 22, 18, RGB(0xb3, 0x32, 0x3c), 255);
      icon_draw_cached(s, IC_INFO, 44, cy + 22, 18, C_WHITE);
    }
    if (cy + bh > top - 40 && cy < bottom + 40)
      draw_bubble(s, &msgs[i], bx, cy, bw, bh);
    cy += bh + 18;
  }

  /* 正在输入 */
  if (typing_on)
  {
    gfx_fill_circle_a(s, 44, cy + 22, 18, RGB(0x6a, 0x45, 0xd8), 255);
    text_draw_vcenter_center(s, 44, cy + 4, 36, "AI", FONT_SMALL, C_WHITE);
    gfx_fill_round_rect(s, 74, cy, waiting ? 216 : 96, 44, 16, C_SURFACE2);
    for (int i = 0; i < 3; i++)
    {
      int ph = (int)((__builtin_sinf((float)(now_ms() % 900) / 900.0f * 6.283f + i * 1.2f) + 1.0f) * 3);
      gfx_fill_circle_a(s, 98 + i * 22, cy + 22 - ph, 5, C_TEXT, 200);
    }
    if (waiting)
      text_draw_vcenter(s, 74 + 74, cy, 44, "正在思考…", FONT_BODY, C_TEXT);
    cy += 62;
  }
  gfx_reset_clip();
}

static void draw_input_bar(Surface *s, int y)
{
  gfx_fill_rect(s, 0, y, SCREEN_W, SCREEN_H - y, C_BG);
  gfx_fill_rect(s, 0, y, SCREEN_W, 1, C_LINE);
  /* 输入框 */
  gfx_fill_round_rect(s, 16, y + 10, SCREEN_W - 108, INPUT_H - 20, (INPUT_H - 20) / 2, C_SURFACE2);
  if (draft[0])
  {
    char show[INPUT_MAX + 8];
    snprintf(show, sizeof(show), "%s", draft);
    show_tail(show, SCREEN_W - 140);
    text_draw_vcenter(s, 34, y + 10, INPUT_H - 20, show, FONT_BODY, C_TEXT);
  }
  else
  {
    text_draw_vcenter(s, 34, y + 10, INPUT_H - 20, kb_open ? "" : "说点什么…（轻触输入）",
                      FONT_BODY, C_TEXT_MUTED);
  }
  /* 发送键 */
  int has = draft[0] != 0 || waiting;
  uint32_t kbg = waiting ? RGB(0xb3, 0x32, 0x3c) : (draft[0] ? C_ACCENT : C_SURFACE2);
  gfx_fill_circle(s, SCREEN_W - 48, y + INPUT_H / 2, 24, kbg);
  icon_draw_cached(s, waiting ? IC_CLOSE : IC_SEND, SCREEN_W - 48, y + INPUT_H / 2, 22,
                   waiting ? C_WHITE : (has ? C_BLACK : C_TEXT_MUTED));
}

/* ---------- 屏幕键盘 ---------- */
/*
 * 键位：从 68x44/间隙6 放大到 74x46/间隙4（手指目标是 12.9mm -> 14mm 宽），
 * 并且**绘制与命中测试共用同一套布局**——之前第 4 行画按 7 列居中、判定按 8 列，
 * 整行错位 37px，"看着点这个键、实际点到旁边"，就是手感差的根因。
 */
#define KEY_W 74
#define KEY_H 46
#define KEY_GAP 4
#define KEY_SLOP 6                 /* 命中时上下左右的容差，消掉键间死区 */
#define KB_TOP (SCREEN_H - 5 * (KEY_H + KEY_GAP) - 12)

#define KB_ROW1 "1234567890"
#define KB_ROW2 "qwertyuiop"
#define KB_ROW3 "asdfghjkl"
#define KB_ROW4 "zxcvbnm"
static const char *kb_rows[4] = {KB_ROW1, KB_ROW2, KB_ROW3, KB_ROW4};

/* 第 4 行占 8 格（前 7 格字母 + 最后 1 格退格），其余行格数 = 字母个数 */
static int kb_ncols(int row) { return row == 3 ? 8 : (int)strlen(kb_rows[row]); }

static int kb_row_y(int row) { return KB_TOP + row * (KEY_H + KEY_GAP); }

/* 取某一格的矩形（绘制与判定都以它为准） */
static void kb_cell(int row, int col, int *x, int *y, int *w, int *h)
{
  int n = kb_ncols(row);
  int total = n * KEY_W + (n - 1) * KEY_GAP;
  int x0 = (SCREEN_W - total) / 2;
  *x = x0 + col * (KEY_W + KEY_GAP);
  *y = kb_row_y(row);
  *w = KEY_W;
  *h = KEY_H;
}

/* 第五行（功能键）同样只此一份表，绘制与判定共用 */
enum
{
  KBAR_SHIFT = 0,
  KBAR_SPACE,
  KBAR_HIDE,
  KBAR_SEND,
  KBAR_LANG,
  KBAR_COUNT
};
static const struct
{
  int x, w;
} kb_bar[KBAR_COUNT] = {
    {8, 92}, {110, 300}, {420, 92}, {522, 128}, {660, 132}};

static int kb_bar_y(void) { return kb_row_y(4); }

/* 落在键盘区域内的按下键（用于高亮反馈）；-1 = 没按在键上 */
static int kb_down_row = -1, kb_down_col = -1;

/* ---------- 候选条（复用输入预览那条 30px 的横条，不额外占版面） ---------- */
typedef struct
{
  int x, w, off;
} CandSlot;

static int cand_layout(CandSlot *out, int max)
{
  int x, n = 0;
  const char *p;
  if (!cand_chars)
    return 0;
  x = 30 + text_width(FONT_BODY, py) + 12;
  p = cand_chars;
  while (*p && n < max)
  {
    int clen = utf8_len((unsigned char)*p);
    char one[8];
    int w;
    memcpy(one, p, (size_t)clen);
    one[clen] = 0;
    w = text_width(FONT_BODY, one) + 16;
    if (x + w > SCREEN_W - 132 - 8)
      break;
    out[n].x = x;
    out[n].w = w;
    out[n].off = (int)(p - cand_chars);
    x += w;
    n++;
    p += clen;
  }
  return n;
}

static void draw_composition(Surface *s)
{
  int y = KB_TOP - 42;
  CandSlot cs[CAND_MAX];
  int n, i;
  gfx_fill_round_rect(s, 16, y, SCREEN_W - 132, 30, 15, C_SURFACE2);
  text_draw_vcenter(s, 30, y, 30, py, FONT_BODY, C_ACCENT);
  n = cand_layout(cs, CAND_MAX);
  for (i = 0; i < n; i++)
  {
    const char *p = cand_chars + cs[i].off;
    int clen = utf8_len((unsigned char)*p);
    char one[8];
    memcpy(one, p, (size_t)clen);
    one[clen] = 0;
    if (i == 0)
      gfx_fill_round_rect(s, cs[i].x, y + 3, cs[i].w, 24, 12, C_ACCENT);
    text_draw_vcenter_center(s, cs[i].x + cs[i].w / 2, y, 30, one, FONT_BODY,
                             i == 0 ? RGB(0x08, 0x2a, 0x20) : C_TEXT);
  }
  if (!n)
    text_draw_vcenter(s, 30 + text_width(FONT_BODY, py) + 12, y, 30, "…", FONT_BODY, C_TEXT_MUTED);
}

static void draw_keyboard(Surface *s)
{
  gfx_fill_rect(s, 0, KB_TOP - 12, SCREEN_W, SCREEN_H - KB_TOP + 12, RGB(0x0a, 0x0d, 0x12));
  gfx_fill_rect(s, 0, KB_TOP - 12, SCREEN_W, 1, C_LINE);

  /* 输入预览：组拼音时显示「拼音 + 候选」，否则显示已输入内容 */
  if (cn_mode && py[0])
  {
    draw_composition(s);
  }
  else if (draft[0])
  {
    char show[INPUT_MAX + 8];
    snprintf(show, sizeof(show), "%s", draft);
    show_tail(show, SCREEN_W - 150);
    gfx_fill_round_rect(s, 16, KB_TOP - 42, SCREEN_W - 132, 30, 15, C_SURFACE2);
    text_draw_vcenter(s, 30, KB_TOP - 42, 30, show, FONT_BODY, C_TEXT);
    gfx_fill_circle(s, SCREEN_W - 44, KB_TOP - 27, 15, C_ACCENT);
    icon_draw_cached(s, IC_SEND, SCREEN_W - 44, KB_TOP - 27, 15, C_BLACK);
  }

  for (int r = 0; r < 4; r++)
  {
    int n = (int)strlen(kb_rows[r]);
    for (int c = 0; c < n; c++)
    {
      int x, y, w, h;
      int col = (r == 3) ? c : c; /* 第4行前 7 格是字母，第 8 格留给退格 */
      kb_cell(r, col, &x, &y, &w, &h);
      char label[2] = {kb_rows[r][c], 0};
      if (kb_shift)
        label[0] = (char)(label[0] - 'a' + 'A');
      /* 按下时高亮：手指按上去立刻有反馈，不必等抬起 */
      int pressed = (kb_down_row == r && kb_down_col == c);
      gfx_fill_round_rect(s, x, y, w, h, 8, pressed ? C_ACCENT : C_SURFACE2);
      text_draw_vcenter_center(s, x + w / 2, y, h, label, FONT_BODY,
                               pressed ? C_BLACK : C_TEXT);
    }
  }
  /* 第四行右侧：退格 */
  int x, y, w, h;
  kb_cell(3, 7, &x, &y, &w, &h);
  {
    int pressed = (kb_down_row == 3 && kb_down_col == 7);
    gfx_fill_round_rect(s, x, y, w, h, 8, pressed ? RGB(0xff, 0x8a, 0x92) : RGB(0x3a, 0x2a, 0x2e));
  }
  icon_draw_cached(s, IC_BACK, x + w / 2, y + h / 2, 22, RGB(0xff, 0xb0, 0xb8));
  /* 第五行：大写 / 空格 / 收起 / 发送 / 中英（与命中测试共用 kb_bar 表） */
  int y5 = kb_bar_y();
  for (int i = 0; i < KBAR_COUNT; i++)
  {
    int bx = kb_bar[i].x, bw = kb_bar[i].w;
    int pressed = (kb_down_row == 4 && kb_down_col == i);
    uint32_t bg = C_SURFACE2;
    uint32_t fg = C_TEXT_DIM;
    const char *label = "";
    FontId f = FONT_BODY;
    if (i == KBAR_SHIFT) { label = kb_shift ? "小写" : "大写"; f = FONT_SMALL; }
    else if (i == KBAR_SPACE) { label = "空格"; fg = C_TEXT; }
    else if (i == KBAR_HIDE) { label = "收起"; }
    else if (i == KBAR_SEND)
    {
      label = waiting ? "取消" : "发送";
      bg = waiting ? RGB(0xb3, 0x32, 0x3c) : (draft[0] ? C_ACCENT : C_SURFACE2);
      fg = waiting ? C_WHITE : (draft[0] ? C_BLACK : C_TEXT_MUTED);
    }
    else { label = cn_mode ? "中" : "En"; bg = cn_mode ? C_ACCENT : C_SURFACE2; fg = cn_mode ? C_BLACK : C_TEXT_DIM; }
    if (pressed) { bg = C_ACCENT; fg = C_BLACK; }
    gfx_fill_round_rect(s, bx, y5, bw, KEY_H, 8, bg);
    text_draw_vcenter_center(s, bx + bw / 2, y5, KEY_H, label, f, fg);
  }
  (void)h;
}

static void send_message(void)
{
  if (!draft[0])
    return;
  add_msg(MSG_ME, draft);
  snprintf(last_user, sizeof(last_user), "%s", draft);
  draft[0] = 0;
  py_clear();
  show_retry = 0;
  if (ai_configured())
  {
    ai_ask(last_user); /* 真模型：异步，结果在 frame() 里收 */
    waiting = 1;
    return;
  }
  /* 演示模式（没配 endpoint）：本地关键词回复 */
  pending_reply = 1;
  typing_until = (int)(now_ms() + 700);
  reply_at = now_ms() + 700;
}

/* 发送键在「请求中」就是取消键：比禁用发送省版面，也让取消有地方点 */
static void send_or_cancel(void)
{
  if (waiting)
  {
    ai_cancel();
    waiting = 0;
    ui_toast("已取消");
    return;
  }
  send_message();
}

/* 重试＝把上一条用户消息原样再发一次。
 * 失败的轮次没写进远端历史，所以不会把同一句话记两遍。 */
static void retry_last(void)
{
  if (!last_user[0] || waiting)
    return;
  /* 把上一条错误气泡撤掉再重试：它是「这一次没成」的产物，
   * 留着会越点越多，而重试的语义就是重来一遍。 */
  if (nmsg > 0 && msgs[nmsg - 1].mine == MSG_ERR)
    nmsg--;
  show_retry = 0;
  ai_ask(last_user);
  waiting = 1;
}

/* 顶栏右侧两颗胶囊：清空 / 重试（命中返回 1） */
static int topbar_hit(const UiEvent *e)
{
  int cw = ui_pill_width("清空");
  int cx0 = SCREEN_W - 14 - cw;
  if (ui_hit(e->x, e->y, cx0, 15, cw, 28))
  {
    if (waiting)
    {
      ai_cancel();
      waiting = 0;
    }
    nmsg = 0;
    ai_history_clear();
    pending_reply = 0;
    typing_until = 0;
    show_retry = 0;
    ui_toast("会话已清空");
    return 1;
  }
  if (show_retry && last_user[0])
  {
    int rw = ui_pill_width("重试");
    if (ui_hit(e->x, e->y, cx0 - 10 - rw, 15, rw, 28))
    {
      retry_last();
      return 1;
    }
  }
  return 0;
}

void app_chat_frame(uint64_t t_ms)
{
  Surface *s = gfx_back();
  if (!s)
    return;
  gfx_reset_clip();
  gfx_fill_rect(s, 0, 0, SCREEN_W, SCREEN_H, C_BG);
  ui_status_bar(s, NULL);

  /* 真模型：收结果（每帧一次；AI_OK/AI_ERR 只在那一帧各报一次） */
  AiState st = ai_poll();
  if (st == AI_OK)
  {
    waiting = 0;
    add_msg(MSG_AI, ai_reply());
  }
  else if (st == AI_ERR)
  {
    waiting = 0;
    add_msg(MSG_ERR, ai_error());
    show_retry = 1;
  }

  char sub[128];
  if (!ai_configured())
    snprintf(sub, sizeof(sub), "小石 · 界面演示版（未配置模型）");
  else if (waiting)
    snprintf(sub, sizeof(sub), "小石 · %s · 正在思考…", ai_model());
  else
    snprintf(sub, sizeof(sub), "小石 · %s", ai_model());
  ui_topbar(s, "AI 助手", sub, 1);

  int cw = ui_pill_width("清空");
  ui_pill(s, SCREEN_W - 14 - cw, 15, "清空", C_SURFACE2, C_TEXT);
  if (show_retry)
  {
    int rw = ui_pill_width("重试");
    ui_pill(s, SCREEN_W - 24 - cw - rw, 15, "重试", RGB(0x3f, 0x6e, 0x8f), C_WHITE);
  }

  /* 演示模式：本地关键词回复到点（只在没配 endpoint 时走） */
  if (!ai_configured() && pending_reply && t_ms >= reply_at)
  {
    pending_reply = 0;
    typing_until = 0;
    add_msg(MSG_AI, chat_reply(msgs[nmsg - 1].text));
  }

  int list_top = TB_H;
  int list_bottom = kb_open ? KB_TOP - 12 : SCREEN_H - INPUT_H;
  draw_messages(s, list_top, list_bottom);
  if (kb_open)
    draw_keyboard(s);
  else
    draw_input_bar(s, SCREEN_H - INPUT_H);

  /* 快捷话题（键盘收起时显示在输入框上方） */
  if (!kb_open && nmsg <= 3)
  {
    static const char *chips[3] = {"你会拍照吗？", "怎么录像？", "现在几点？"};
    int x = 16;
    for (int i = 0; i < 3; i++)
    {
      int w = ui_pill_width(chips[i]);
      gfx_fill_round_rect(s, x, SCREEN_H - INPUT_H - 38, w, 30, 15, C_SURFACE);
      text_draw_vcenter_center(s, x + w / 2, SCREEN_H - INPUT_H - 38, 30, chips[i], FONT_SMALL, C_ACCENT);
      x += w + 8;
    }
  }
}

/* 键盘命中测试 */
/*
 * 坐标 -> (行,列)。行与行之间、键与键之间的缝隙都算进"最近的键"，
 * 手指落在缝里也能按到键——密集小目标最怕的死区就是这么来的。
 * 返回 0 = 没落在键盘上。
 */
static int kb_pick(int px, int py, int *row, int *col)
{
  for (int r = 0; r < 4; r++)
  {
    int y0 = kb_row_y(r);
    if (py < y0 - KEY_SLOP || py >= y0 + KEY_H + KEY_SLOP)
      continue;
    int n = kb_ncols(r);
    int total = n * KEY_W + (n - 1) * KEY_GAP;
    int x0 = (SCREEN_W - total) / 2;
    if (px < x0 - KEY_SLOP || px >= x0 + total + KEY_SLOP)
      continue;
    int c = (px - x0 + KEY_GAP / 2) / (KEY_W + KEY_GAP); /* 间隙并入相邻格 */
    if (c < 0)
      c = 0;
    if (c >= n)
      c = n - 1;
    *row = r;
    *col = c;
    return 1;
  }
  int y5 = kb_bar_y();
  if (py >= y5 - KEY_SLOP && py < y5 + KEY_H + KEY_SLOP)
  {
    for (int i = 0; i < KBAR_COUNT; i++)
    {
      if (px >= kb_bar[i].x - KEY_SLOP && px < kb_bar[i].x + kb_bar[i].w + KEY_SLOP)
      {
        *row = 4;
        *col = i;
        return 1;
      }
    }
  }
  return 0;
}

static int kb_hit(const UiEvent *e)
{

  int r = -1, c = -1;
  if (!kb_pick(e->x, e->y, &r, &c))
  {
    /* 键盘上方输入预览条上的发送键 */
    if (draft[0] && ui_hit(e->x, e->y, SCREEN_W - 62, KB_TOP - 45, 40, 36))
    {
      send_or_cancel();
      return 1;
    }
    return 0;
  }
  if (r == 4)
  {
    switch (c)
    {
    case KBAR_SHIFT:
      kb_shift = !kb_shift;
      return 1;
    case KBAR_SPACE:
      if (cn_mode && py[0])
      {
        /* 空格 = 取第一个候选（拼音没收完就不插空格） */
        if (cand_chars)
        {
          char one[8];
          int clen = utf8_len((unsigned char)*cand_chars);
          memcpy(one, cand_chars, (size_t)clen);
          one[clen] = 0;
          py_commit(one);
        }
        return 1;
      }
      draft_append(" ");
      return 1;
    case KBAR_HIDE:
      kb_open = 0;
      py_clear();
      kb_down_row = kb_down_col = -1;
      return 1;
    case KBAR_SEND:
      send_or_cancel();
      return 1;
    case KBAR_LANG:
      cn_mode = !cn_mode;
      py_clear();
      return 1;
    }
    return 0;
  }
  if (r == 3 && c == 7)
  { /* 退格：拼音模式下先退拼音 */
    if (cn_mode && py[0])
    {
      int n = (int)strlen(py);
      py[n - 1] = 0;
      py_refresh();
    }
    else
    {
      draft_backspace();
    }
    return 1;
  }
  {
    char ch = kb_rows[r][c];
    char one[2];
    if (cn_mode && r > 0)
    {
      py_key(ch); /* 字母行进拼音 */
      return 1;
    }
    if (!cn_mode && kb_shift && ch >= 'a' && ch <= 'z')
      ch = (char)(ch - 'a' + 'A');
    one[0] = ch;
    one[1] = 0;
    draft_append(one);
    return 1;
  }
  return 0;
}

int app_chat_event(const UiEvent *e)
{
  if (kb_open)
  {
    if (e->type == UI_EV_UP)
    {
      kb_down_row = kb_down_col = -1; /* 高亮先清掉：下面 tap 分支会提前 return */
      if (e->tap)
      {
        if (ui_topbar_back_hit(e->x, e->y))
        {
          kb_open = 0;
          return 0;
        }
        if (topbar_hit(e))
          return 0; /* 清空 / 重试（返回 1 = 回桌面，别踩） */
        /* 键盘上方的聊天区不处理；但候选条就在键盘上面那条 30px 里，要能点 */
        if (e->y < KB_TOP - 12 && !(cn_mode && py[0] && e->y >= KB_TOP - 44))
          return 0;
        kb_hit(e);
        return 0;
      }
    }
    if (e->type == UI_EV_DOWN && e->y >= KB_TOP - 12)
    {
      /* 键盘上的按下：先记下按到的键用于高亮（动作仍在抬起时执行） */
      kb_down_row = kb_down_col = -1;
      kb_pick(e->x, e->y, &kb_down_row, &kb_down_col);
      return 0;
    }
    if (e->type == UI_EV_MOVE && kb_down_row >= 0)
    {
      kb_down_row = kb_down_col = -1;
      kb_pick(e->x, e->y, &kb_down_row, &kb_down_col);
      return 0;
    }
    if (e->type == UI_EV_DOWN && e->y > TB_H && e->y < KB_TOP - 12)
    {
      dragging = 1;
      drag_y0 = e->y;
      drag_scroll0 = scroll;
    }
    if (e->type == UI_EV_MOVE && dragging)
    {
      scroll = drag_scroll0 - (e->y - drag_y0);
      if (scroll < 0)
        scroll = 0;
    }
    if (e->type == UI_EV_UP)
      dragging = 0;
    return 0;
  }

  if (e->type == UI_EV_DOWN)
  {
    if (e->y > TB_H && e->y < SCREEN_H - INPUT_H)
    {
      dragging = 1;
      drag_y0 = e->y;
      drag_scroll0 = scroll;
    }
    return 0;
  }
  if (e->type == UI_EV_MOVE && dragging)
  {
    scroll = drag_scroll0 - (e->y - drag_y0);
    if (scroll < 0)
      scroll = 0;
    return 0;
  }
  if (e->type == UI_EV_UP)
  {
    dragging = 0;
    /* e->tap 已经是「按下到抬起没怎么动」的判定，不用再看 dragging：
     * 这块区域按下就会置 dragging，原先的 !was_drag 让下面的快捷话题永远点不到。 */
    if (!e->tap)
      return 0;
    if (ui_topbar_back_hit(e->x, e->y))
      return 1;
    if (topbar_hit(e))
      return 0; /* 同上：处理完留在聊天界面 */

    /* 输入框 -> 打开键盘 */
    if (e->y >= SCREEN_H - INPUT_H)
    {
      if (ui_hit(e->x, e->y, 16, SCREEN_H - INPUT_H + 10, SCREEN_W - 108, INPUT_H - 20))
      {
        kb_open = 1;
        return 0;
      }
      if (ui_hit(e->x, e->y, SCREEN_W - 76, SCREEN_H - INPUT_H + 4, 56, 48))
      {
        send_or_cancel();
        return 0;
      }
      return 0;
    }
    /* 快捷话题 */
    if (nmsg <= 3 && e->y >= SCREEN_H - INPUT_H - 40 && e->y < SCREEN_H - INPUT_H - 4)
    {
      static const char *chips[3] = {"你会拍照吗？", "怎么录像？", "现在几点？"};
      int x = 16;
      for (int i = 0; i < 3; i++)
      {
        int w = ui_pill_width(chips[i]);
        if (ui_hit(e->x, e->y, x, SCREEN_H - INPUT_H - 38, w, 30))
        {
          snprintf(draft, sizeof(draft), "%s", chips[i]);
          send_or_cancel();
          return 0;
        }
        x += w + 8;
      }
    }
    return 0;
  }
  return 0;
}

const AppDef g_app_chat = {
    .title = "AI聊天",
    .en = "AI Chat",
    .icon = APPICON_CHAT,
    .needs_camera = 0,
    .enter = app_chat_enter,
    .leave = app_chat_leave,
    .frame = app_chat_frame,
    .event = app_chat_event};
