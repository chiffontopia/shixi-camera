/*
 * app_brick.c — 打砖块（shixi 栈落地）
 *
 * 规则与物理常数来自 design/13-brick-breaker.md（工单 13 已定死）。
 * 与 design/13 的差异全部源于"不再用 LVGL"，逐条对应：
 *
 *   1. 渲染载体：不用对象树/脏矩形 —— 本栈本来就是"每帧整屏重画 + 双缓冲翻页"
 *      （README 实现要点），四个既有应用都这么画，所以走同一条路，不引入第二套机制。
 *      每帧成本估算见 README「打砖块」一节。
 *   2. 跟手：input.c 是事件式的（没有 lv_indev 那种"当前按压点"查询），
 *      所以这里自己记住手指最后位置，每帧把挡板贴过去。手指不动就不发事件，
 *      挡板自然也不动 —— 与 design/13 §5 的"绝对跟手"等价。
 *      游戏区判定用 `y >= PLAY_Y0`，所以顶栏上的按钮不会把挡板吸走。
 *   3. HUD：直接用本栈统一的 ui_topbar（58px）+ 一个「暂停/继续」按钮，
 *      于是 PLAY_Y0 = UI_TOPBAR_H（design 里写 64，是因为那边自绘 HUD）。
 *   4. 结算面板：gfx/ui 原语手绘（ui_card + ui_button + ui_hit），不用 lv_obj。
 *   5. 定时：main.c 的 tick 已按 33ms 调 frame(t_ms)，8ms 定步长 + 累加器照搬。
 *   6. 暂停：design 用 lv_timer_pause，这里用状态位挡住物理（frame 照常画）。
 *
 * 演示模式（无人值守）：SHIXI_BRICK_DEMO=1 全程自动跟球；=<毫秒> 演示这么久后交还手动。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "apps.h"
#include "ui.h"
#include "media.h"
#include "util.h"

/* ---------------- 场地 ---------------- */
#define PLAY_X0   0
#define PLAY_X1   SCREEN_W
#define PLAY_Y0   UI_TOPBAR_H          /* 58：HUD 用统一顶栏，游戏区从它下面开始 */
#define PLAY_Y1   SCREEN_H
/* ---------------- 砖块（design/13 §1） ---------------- */
#define BRICK_ROWS 5
#define BRICK_COLS 8
#define BRICK_W    88
#define BRICK_H    26
#define BRICK_GAP  8
#define BRICK_X0   20
#define BRICK_Y0   80
/* ---------------- 挡板与球 ---------------- */
#define PADDLE_W   120
#define PADDLE_H   16
#define PADDLE_Y   452
#define BALL_W     18
#define BALL_H     18
/* ---------------- 时间与速度 ---------------- */
#define STEP_MS          8
#define MAX_CATCHUP_MS   100
#define SPEED_BASE       260.f
#define SPEED_MAX        600.f
#define SPEED_UP         1.10f
#define MAX_DEFLECT_DEG  60.f
#define PI_F             3.14159265f
/* ---------------- 规则 ---------------- */
#define SCORE_PER_BRICK  10
#define LEVELS           3
/* ---------------- HUD ---------------- */
#define HUD_BTN_W 76
#define HUD_BTN_H 40
#define HUD_BTN_X (SCREEN_W - HUD_BTN_W - 12)
#define HUD_BTN_Y ((UI_TOPBAR_H - HUD_BTN_H) / 2)

/* ---------------- 结算面板 ---------------- */
#define PANEL_W   460
#define PANEL_H   300
#define PANEL_X   ((SCREEN_W - PANEL_W) / 2)
#define PANEL_Y   90
#define PBTN_W    200
#define PBTN_H    54
#define PBTN_Y    (PANEL_Y + 212)
#define PBTN_RE_X (PANEL_X + 20)
#define PBTN_HOME_X (PANEL_X + PANEL_W - 20 - PBTN_W)

#define BEST_FILE "brick_best.txt"

typedef enum { ST_READY, ST_RUNNING, ST_PAUSED, ST_OVER } BState;

static BState state = ST_READY;
static int    brick[BRICK_ROWS][BRICK_COLS];
static int    bricks_left;
static float  ball_x, ball_y, bvx, bvy, speed;   /* 球：左上角坐标 + 速度(px/s) */
static float  paddle_cx;
static int    score, best, best0, level, is_record;

static uint64_t last_t;
static int      acc_ms;

/* 手指最后位置（事件式输入的"绝对跟手"实现，见文件头 §2） */
static int touch_down, touch_x, touch_y;

/* 演示模式：0=关闭，>0=剩余毫秒，-1=全程 */
static int demo_left;

/* ---------------- 布局换算 ---------------- */
static int brick_x(int j) { return BRICK_X0 + j * (BRICK_W + BRICK_GAP); }
static int brick_y(int i) { return BRICK_Y0 + i * (BRICK_H + BRICK_GAP); }

/* 每行一个颜色，取自 shixi 调色板（gfx.h），按行区分层次 */
static uint32_t brick_color(int row)
{
    switch (row) {
    case 0:  return RGB(0xff, 0x6b, 0x6b);   /* 红 */
    case 1:  return RGB(0xff, 0xb0, 0x20);   /* 橙 */
    case 2:  return RGB(0x2e, 0xd3, 0xa8);   /* 薄荷绿（主色） */
    case 3:  return RGB(0x4c, 0x8d, 0xff);   /* 蓝 */
    default: return RGB(0xa9, 0x8b, 0xff);   /* 紫 */
    }
}

/* ---------------- 最佳分落盘 ---------------- */
static void best_path(char *buf, int n)
{
    const char *env = getenv("BRICK_BEST");
    if (env && *env) { snprintf(buf, n, "%s", env); return; }
    snprintf(buf, n, "%s/" BEST_FILE, media_root());
}

static int best_load(void)
{
    char p[512];
    best_path(p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (!f) return 0;                 /* 首次运行：没有文件就是 0，不报警 */
    int v = 0;
    if (fscanf(f, "%d", &v) != 1 || v < 0) v = 0;   /* 内容坏了也当 0 */
    fclose(f);
    return v;
}

static void best_store(int v)
{
    char p[512];
    best_path(p, sizeof(p));
    FILE *f = fopen(p, "w");
    if (!f) { fprintf(stderr, "打砖块：最佳分写不进去 %s\n", p); return; }  /* 写失败只记一行 */
    fprintf(f, "%d\n", v);
    fclose(f);
}

/* ---------------- 局初始化 ---------------- */
static void build_bricks(void)
{
    for (int i = 0; i < BRICK_ROWS; i++)
        for (int j = 0; j < BRICK_COLS; j++) brick[i][j] = 1;
    bricks_left = BRICK_ROWS * BRICK_COLS;
}

static void reset_ball(void)
{
    ball_x = paddle_cx - BALL_W / 2.f;
    ball_y = PADDLE_Y - BALL_H;
    bvx = 0.f;
    bvy = 0.f;
    acc_ms = 0;
    state = ST_READY;
}

static void launch(void)
{
    float ang = 60.f * PI_F / 180.f;
    bvx = -speed * sinf(ang);         /* 向左上（design/13 §2） */
    bvy = -speed * cosf(ang);
    ball_x = paddle_cx - BALL_W / 2.f;
    ball_y = PADDLE_Y - BALL_H;
    state = ST_RUNNING;
    acc_ms = 0;
}

static void next_level(void)
{
    level = level % LEVELS + 1;                        /* 3 关循环 */
    speed *= SPEED_UP;
    if (speed > SPEED_MAX) speed = SPEED_MAX;
    build_bricks();
    reset_ball();                                      /* 回 READY，重新发球 */
}

static void game_over(void)
{
    state = ST_OVER;
    touch_down = 0;
    is_record = score > best0;
    if (score > best) {                                /* 一局只写一次 */
        best = score;
        best_store(best);
    }
}

/* ---------------- 物理 ---------------- */
static void hit_bricks(void)
{
    for (int i = 0; i < BRICK_ROWS; i++) {
        for (int j = 0; j < BRICK_COLS; j++) {
            if (!brick[i][j]) continue;
            float bx = (float)brick_x(j), by = (float)brick_y(i);
            if (ball_x + BALL_W <= bx || ball_x >= bx + BRICK_W) continue;
            if (ball_y + BALL_H <= by || ball_y >= by + BRICK_H) continue;

            /* 反弹轴取穿透更浅的那一轴，并把球推回不重叠位置 */
            float pen_l = (ball_x + BALL_W) - bx;
            float pen_r = (bx + BRICK_W) - ball_x;
            float pen_t = (ball_y + BALL_H) - by;
            float pen_b = (by + BRICK_H) - ball_y;
            float pen_x = pen_l < pen_r ? pen_l : pen_r;
            float pen_y = pen_t < pen_b ? pen_t : pen_b;
            if (pen_x < pen_y) {
                if (pen_l < pen_r) ball_x = bx - BALL_W; else ball_x = bx + BRICK_W;
                bvx = -bvx;
            } else {
                if (pen_t < pen_b) ball_y = by - BALL_H; else ball_y = by + BRICK_H;
                bvy = -bvy;
            }

            brick[i][j] = 0;
            bricks_left--;
            score += SCORE_PER_BRICK * level;
            if (bricks_left == 0) next_level();
            return;                     /* 每子步最多碎一块 */
        }
    }
}

static void hit_paddle(void)
{
    float px = paddle_cx - PADDLE_W / 2.f;
    if (ball_x + BALL_W <= px || ball_x >= px + PADDLE_W) return;
    if (ball_y + BALL_H < PADDLE_Y || ball_y > PADDLE_Y + PADDLE_H) return;

    float rel = (ball_x + BALL_W / 2.f - paddle_cx) / (PADDLE_W / 2.f);
    if (rel < -1.f) rel = -1.f;
    if (rel > 1.f) rel = 1.f;
    float ang = rel * (MAX_DEFLECT_DEG * PI_F / 180.f);
    bvx = speed * sinf(ang);            /* 速率不变，只改方向 */
    bvy = -speed * cosf(ang);
    ball_y = PADDLE_Y - BALL_H;         /* 贴到挡板上沿，防同一子步重复判定 */
}

static void step(void)
{
    float s = STEP_MS / 1000.f;
    ball_x += bvx * s;
    ball_y += bvy * s;

    /* 左右墙与顶墙（顶墙 = HUD 下沿） */
    if (ball_x < PLAY_X0) { ball_x = PLAY_X0; bvx = -bvx; }
    if (ball_x + BALL_W > PLAY_X1) { ball_x = PLAY_X1 - BALL_W; bvx = -bvx; }
    if (ball_y < PLAY_Y0) { ball_y = PLAY_Y0; bvy = -bvy; }

    hit_bricks();
    if (state != ST_RUNNING) return;    /* 过关了：球已回位 */

    if (bvy > 0.f) hit_paddle();

    if (ball_y + BALL_H >= PLAY_Y1) {   /* 漏球：一条命，直接结束 */
        ball_y = PLAY_Y1 - BALL_H;
        game_over();
    }
}

/* ---------------- 输入 ---------------- */
static void update_paddle(void)
{
    if (!touch_down || touch_y < PLAY_Y0) return;       /* 顶栏按压不动挡板 */
    float cx = (float)touch_x;
    if (cx < PADDLE_W / 2.f) cx = PADDLE_W / 2.f;
    if (cx > PLAY_X1 - PADDLE_W / 2.f) cx = PLAY_X1 - PADDLE_W / 2.f;
    paddle_cx = cx;
}

/* ---------------- 演示模式 ---------------- */
/* 挑「最低一行里离球最近的活砖」当目标：清场快，也像真的在打（而不是只跟着球跑） */
static int aim_target_x(float bcx)
{
    for (int i = BRICK_ROWS - 1; i >= 0; i--) {
        int best = -1;
        float bd = 1e9f;
        for (int j = 0; j < BRICK_COLS; j++) {
            if (!brick[i][j]) continue;
            float d = fabsf((float)(brick_x(j) + BRICK_W / 2) - bcx);
            if (d < bd) { bd = d; best = j; }
        }
        if (best >= 0) return brick_x(best) + BRICK_W / 2;
    }
    return SCREEN_W / 2;                 /* 砖打完了：随便回中 */
}

static void demo_step(int dt)
{
    if (demo_left == 0) return;
    if (state == ST_READY) launch();
    if (state != ST_RUNNING) return;

    /* 把挡板中心故意偏离球心，出射角就朝目标那边（|偏移| 远小于半块挡板，接得住） */
    float bcx = ball_x + BALL_W / 2.f;
    float off = (aim_target_x(bcx) > bcx) ? PADDLE_W / 4.f : -(PADDLE_W / 4.f);
    touch_down = 1;
    touch_x = (int)(bcx - off);
    touch_y = PADDLE_Y + 8;

    if (demo_left > 0) {
        demo_left -= dt;
        if (demo_left <= 0) { demo_left = 0; touch_down = 0; }   /* 交还手动 */
    }
}

/* ---------------- 绘制 ---------------- */
static void draw_hud(Surface *s)
{
    char sub[64], b[32];
    snprintf(sub, sizeof(sub), "关卡 %d · 得分 %d", level, score);
    ui_topbar(s, "打砖块", sub, 1);

    snprintf(b, sizeof(b), "最佳 %d", best);
    int bw = text_width(FONT_SMALL, b);
    text_draw_vcenter(s, HUD_BTN_X - 16 - bw, UI_TOPBAR_H / 2 - 2, UI_TOPBAR_H / 2,
                      b, FONT_SMALL, C_TEXT_MUTED);

    ui_button(s, HUD_BTN_X, HUD_BTN_Y, HUD_BTN_W, HUD_BTN_H,
              state == ST_PAUSED ? "继续" : "暂停", C_SURFACE2, C_TEXT, 0, 10);
}

static void draw_panel(Surface *s)
{
    ui_dimmer(s, 170);
    ui_card(s, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 22);
    text_draw_center(s, SCREEN_W / 2, PANEL_Y + 22, "本局结束", FONT_TITLE, C_TEXT);

    char big[32];
    snprintf(big, sizeof(big), "%d", score);
    text_draw_center(s, SCREEN_W / 2, PANEL_Y + 66, big, FONT_HUGE, C_ACCENT);
    text_draw_center(s, SCREEN_W / 2, PANEL_Y + 122, "本局得分", FONT_SMALL, C_TEXT_MUTED);

    char bb[48];
    if (is_record) {
        snprintf(bb, sizeof(bb), "最佳 %d  ↑", best);
        text_draw_center(s, SCREEN_W / 2, PANEL_Y + 148, bb, FONT_BODY, C_WARN);
        const char *tag = "新纪录";
        int tw = ui_pill_width(tag);
        ui_pill(s, (SCREEN_W - tw) / 2, PANEL_Y + 180, tag, C_WARN, C_BLACK);
    } else {
        snprintf(bb, sizeof(bb), "最佳 %d", best);
        text_draw_center(s, SCREEN_W / 2, PANEL_Y + 152, bb, FONT_BODY, C_TEXT_DIM);
    }

    ui_button(s, PBTN_RE_X, PBTN_Y, PBTN_W, PBTN_H, "再来一局", C_ACCENT, C_BLACK, 0, 14);
    ui_button(s, PBTN_HOME_X, PBTN_Y, PBTN_W, PBTN_H, "返回桌面", C_SURFACE2, C_TEXT, 0, 14);
}

static void draw(uint64_t t_ms)
{
    Surface *s = gfx_back();
    if (!s) return;
    gfx_reset_clip();

    /* 场地底色 + 左右墙 */
    gfx_fill_rect(s, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    gfx_fill_rect(s, PLAY_X0, PLAY_Y0, 2, PLAY_Y1 - PLAY_Y0, C_LINE);
    gfx_fill_rect(s, PLAY_X1 - 2, PLAY_Y0, 2, PLAY_Y1 - PLAY_Y0, C_LINE);

    /* 砖块：圆角块 + 顶部高光 */
    for (int i = 0; i < BRICK_ROWS; i++) {
        for (int j = 0; j < BRICK_COLS; j++) {
            if (!brick[i][j]) continue;
            int x = brick_x(j), y = brick_y(i);
            uint32_t c = brick_color(i);
            gfx_fill_round_rect(s, x, y, BRICK_W, BRICK_H, 7, c);
            gfx_fill_round_rect(s, x + 4, y + 3, BRICK_W - 8, 4, 2,
                                gfx_blend(c, 0xffffff, 90));
        }
    }

    /* 挡板 */
    int px = (int)(paddle_cx - PADDLE_W / 2.f);
    gfx_fill_round_rect(s, px, PADDLE_Y, PADDLE_W, PADDLE_H, 8, C_ACCENT);
    gfx_fill_round_rect(s, px + 8, PADDLE_Y + 3, PADDLE_W - 16, 3, 2,
                        gfx_blend(C_ACCENT, 0xffffff, 110));

    /* 球（碰撞用方形 AABB，画成圆形更好看） */
    gfx_fill_circle(s, (int)ball_x + BALL_W / 2, (int)ball_y + BALL_H / 2, BALL_W / 2, C_WHITE);
    gfx_fill_circle(s, (int)ball_x + BALL_W / 2 - 2, (int)ball_y + BALL_H / 2 - 2, 2,
                    RGB(0xd2, 0xf2, 0xea));

    draw_hud(s);

    if (state == ST_READY)
        text_draw_vcenter_center(s, SCREEN_W / 2, 300, 40, "轻触游戏区发球", FONT_BODY, C_TEXT_DIM);
    else if (state == ST_PAUSED) {
        ui_dimmer(s, 150);
        text_draw_vcenter_center(s, SCREEN_W / 2, 200, 60, "已暂停", FONT_TITLE, C_TEXT);
        text_draw_vcenter_center(s, SCREEN_W / 2, 262, 40, "点右上角「继续」",
                                 FONT_SMALL, C_TEXT_DIM);
    } else if (state == ST_OVER)
        draw_panel(s);

    (void)t_ms;
}

/* ---------------- AppDef 四回调 ---------------- */
static void app_brick_enter(void)
{
    best0 = best_load();
    best = best0;
    score = 0;
    level = 1;
    speed = SPEED_BASE;
    is_record = 0;
    paddle_cx = SCREEN_W / 2.f;
    touch_down = 0;
    touch_y = 0;

    const char *d = getenv("SHIXI_BRICK_DEMO");
    int dv = (d && *d) ? atoi(d) : 0;
    demo_left = (dv == 0) ? 0 : (dv == 1 ? -1 : dv);   /* 0/未设=关闭，1=全程演示，>1=演示这么多毫秒 */

    build_bricks();
    reset_ball();
    last_t = now_ms();
    acc_ms = 0;
}

static void app_brick_leave(void)
{
    if (state != ST_OVER) game_over();   /* 返回 = 放弃本局，最高分照样落盘 */
}

static void app_brick_frame(uint64_t t_ms)
{
    uint64_t dt64 = t_ms - last_t;
    last_t = t_ms;
    int dt = dt64 > MAX_CATCHUP_MS ? MAX_CATCHUP_MS : (int)dt64;   /* 卡顿/时钟跳变不追 */

    demo_step(dt);

    if (state == ST_RUNNING) {
        update_paddle();                       /* 先跟手，再跑物理（design/13 §3） */
        acc_ms += dt;
        while (acc_ms >= STEP_MS) {
            acc_ms -= STEP_MS;
            step();
            if (state != ST_RUNNING) break;
        }
    } else if (state == ST_READY) {
        update_paddle();
        ball_x = paddle_cx - BALL_W / 2.f;     /* 发球前球贴在挡板上 */
        ball_y = PADDLE_Y - BALL_H;
    }

    draw(t_ms);
}

static int app_brick_event(const UiEvent *e)
{
    if (e->type == UI_EV_DOWN) {
        if (e->y < PLAY_Y0) return 0;          /* HUD：只认按钮，不碰挡板 */
        touch_down = 1;
        touch_x = e->x;
        touch_y = e->y;
        if (state == ST_READY) launch();
        return 0;
    }
    if (e->type == UI_EV_MOVE) {
        if (touch_down) { touch_x = e->x; touch_y = e->y; }
        return 0;
    }
    if (e->type == UI_EV_UP) {
        touch_down = 0;
        if (!e->tap) return 0;
        if (ui_topbar_back_hit(e->x, e->y)) return 1;    /* 返回 = 放弃本局（leave 落盘） */
        if (ui_hit_pad(e->x, e->y, HUD_BTN_X, HUD_BTN_Y, HUD_BTN_W, HUD_BTN_H, 6)) {
            if (state == ST_RUNNING) { state = ST_PAUSED; touch_down = 0; }
            else if (state == ST_PAUSED) { state = ST_RUNNING; acc_ms = 0; }
            return 0;
        }
        if (state == ST_OVER) {
            if (ui_hit(e->x, e->y, PBTN_RE_X, PBTN_Y, PBTN_W, PBTN_H)) {
                app_brick_enter();               /* 再来一局 */
            } else if (ui_hit(e->x, e->y, PBTN_HOME_X, PBTN_Y, PBTN_W, PBTN_H)) {
                return 1;                        /* 返回桌面 */
            }
        }
        return 0;
    }
    return 0;
}

const AppDef g_app_brick = {
    .title = "打砖块",
    .en = "Brick Breaker",
    .icon = APPICON_BRICK,
    .needs_camera = 0,
    .enter = app_brick_enter,
    .leave = app_brick_leave,
    .frame = app_brick_frame,
    .event = app_brick_event
};
