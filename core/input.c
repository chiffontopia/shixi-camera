/*
 * input.c — evdev 触摸输入 + 手势识别 + 调试注入
 */
#include "input.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <sys/ioctl.h>

#define CTRL_FIFO "/tmp/shixi_ctrl"
/*
 * 判定参数（实测这块 gslX680 电容屏抖动不小，参数要留余量）：
 *   - 手指点一下的过程中坐标常会漂 20~60 原始值（≈16~47 屏幕像素），
 *     阈值太紧会把"点击"误判成"滑动"，表现就是点了没反应；
 *   - 按得慢一点（>700ms）以前也不算点击，同样会点不动。
 * 现在的规则：
 *   位移 < SWIPE_MIN            -> 点击（快慢都算）
 *   位移 < SWIPE_MIN*2 且按住 >=350ms -> 也算点击（慢慢按、带抖动）
 *   否则按主轴方向判为滑动
 */
#define SWIPE_MIN      46     /* 净位移小于它就一定是点击（快慢都算） */
#define TAP_SLOW_MS    250    /* 按住超过这个时长、且没跑远 -> 仍算点击（抖动手指出不去） */
#define TAP_MOVE_MAX   100    /* 慢速点击允许的最大净位移（真手指抖动 20~60 原始值很常见） */
#define TAP_TIME_MAX   1200   /* 点击最长按下时间(ms) */
#define LONG_PRESS_MS  650    /* 长按触发时间（当前界面没用到，保留） */
#define MOVE_EMIT_MIN  3      /* 坐标变化超过该值才上报 MOVE，避免洪水 */

static int   fd = -1;
static char  dev_name[64] = "(未打开)";
static int   x_min = 0, x_max = 1023, y_min = 0, y_max = 599;
static int   swap_xy = 0, invert_x = 0, invert_y = 0;

/* 手势状态 */
static int   down = 0;
static int   pending_down = 0;      /* 等坐标到齐再上报 DOWN（避免用上一次的旧坐标） */
static int   coords_pkt = 0;        /* **当前事件包**里出现过坐标 */
static int   down_emitted = 0;      /* 本次触摸的 DOWN 是否已经发出去 */
static int   last_mx = -1, last_my = -1;  /* 上次上报 MOVE 的位置（去重用） */
static int   touch_debug = 0;       /* SHIXI_TOUCH_DEBUG=1 打印每次手势的判定依据 */
static int   cx, cy, sx, sy;
static uint64_t down_ms = 0;
static int   moved = 0;
static int   long_fired = 0;

/* 调试通道 */
static int   ctrl_fd = -1;
static int   debug_on = 0;
static char  ctrl_buf[256];
static int   ctrl_len = 0;
/* 注入队列 */
static UiEvent inj_q[32];
static int inj_head = 0, inj_tail = 0;

static int map_x(int raw)
{
    int v = raw;
    if (x_max == x_min) return 0;
    v = (int)((long)(raw - x_min) * 799 / (x_max - x_min));
    if (invert_x) v = 799 - v;
    return iclamp(v, 0, 799);
}

static int map_y(int raw)
{
    int v = raw;
    if (y_max == y_min) return 0;
    v = (int)((long)(raw - y_min) * 479 / (y_max - y_min));
    if (invert_y) v = 479 - v;
    return iclamp(v, 0, 479);
}

int input_ready(void) { return fd >= 0; }
const char *input_device_name(void) { return dev_name; }

void input_debug_enable(int on)
{
    debug_on = on;
    if (on && ctrl_fd < 0) {
        /* 非阻塞打开已有的 fifo，不存在则创建 */
        mkfifo(CTRL_FIFO, 0666);
        ctrl_fd = open(CTRL_FIFO, O_RDONLY | O_NONBLOCK);
    }
}

int input_init(void)
{
    const char *path = getenv("SHIXI_TOUCH_DEV");
    if (!path || !*path) path = "/dev/input/event0";
    if (getenv("SHIXI_TOUCH_SWAP"))     swap_xy  = atoi(getenv("SHIXI_TOUCH_SWAP"));
    if (getenv("SHIXI_TOUCH_INVERT_X")) invert_x = atoi(getenv("SHIXI_TOUCH_INVERT_X"));
    if (getenv("SHIXI_TOUCH_INVERT_Y")) invert_y = atoi(getenv("SHIXI_TOUCH_INVERT_Y"));

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        snprintf(dev_name, sizeof(dev_name), "(打开 %s 失败)", path);
        fprintf(stderr, "input: 打开触摸屏 %s 失败: %s\n", path, strerror(errno));
        return -1;
    }
    char name[64] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) > 0)
        snprintf(dev_name, sizeof(dev_name), "%s", name);
    else
        snprintf(dev_name, sizeof(dev_name), "%s", path);

    struct input_absinfo ai;
    if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) { x_min = ai.minimum; x_max = ai.maximum; }
    if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) { y_min = ai.minimum; y_max = ai.maximum; }
    if (x_max <= x_min) { x_min = 0; x_max = 1023; }
    if (y_max <= y_min) { y_min = 0; y_max = 599; }

    fprintf(stderr, "input: 触摸设备 %s [%s] X:%d..%d Y:%d..%d\n",
            path, dev_name, x_min, x_max, y_min, y_max);

    if (getenv("SHIXI_DEBUG_INPUT")) input_debug_enable(1);
    if (getenv("SHIXI_TOUCH_DEBUG")) {
        touch_debug = 1;
        fprintf(stderr, "input: 触摸调试已开启（打印每次手势的位移/时长/判定）\n");
    }
    return 0;
}

/* ------------ 调试通道 ------------ */
static void inj_push(const UiEvent *e)
{
    int next = (inj_tail + 1) % (int)(sizeof(inj_q) / sizeof(inj_q[0]));
    if (next == inj_head) return;   /* 队列满，丢弃 */
    inj_q[inj_tail] = *e;
    inj_tail = next;
}

int input_inject_cmd(const char *cmd)
{
    char what[16] = {0};
    int a = 0, b = 0, c = 0, d = 0;
    int n = sscanf(cmd, "%15s %d %d %d %d", what, &a, &b, &c, &d);
    if (n < 1) return -1;
    UiEvent e;
    memset(&e, 0, sizeof(e));
    if (!strcmp(what, "tap") && n >= 3) {
        /* 模拟真实触摸：先按下再抬起 */
        UiEvent d;
        memset(&d, 0, sizeof(d));
        d.type = UI_EV_DOWN; d.x = d.x0 = iclamp(a, 0, 799); d.y = d.y0 = iclamp(b, 0, 479);
        inj_push(&d);
        e.type = UI_EV_UP; e.x = e.x0 = iclamp(a, 0, 799); e.y = e.y0 = iclamp(b, 0, 479);
        e.tap = 1; e.dur_ms = 60; e.swipe = SWIPE_NONE;
        inj_push(&e);
        return 0;
    }
    if (!strcmp(what, "down") && n >= 3) {
        e.type = UI_EV_DOWN; e.x = e.x0 = a; e.y = e.y0 = b;
        inj_push(&e);
        return 0;
    }
    if (!strcmp(what, "move") && n >= 3) {
        e.type = UI_EV_MOVE; e.x = a; e.y = b; e.x0 = a; e.y0 = b;
        inj_push(&e);
        return 0;
    }
    if (!strcmp(what, "up") && n >= 3) {
        e.type = UI_EV_UP; e.x = e.x0 = a; e.y = e.y0 = b; e.tap = 1; e.dur_ms = 60;
        inj_push(&e);
        return 0;
    }
    if (!strcmp(what, "swipe") && n >= 5) {
        int dx = c - a, dy = d - b;
        e.type = UI_EV_UP; e.x = c; e.y = d; e.x0 = a; e.y0 = b; e.dur_ms = 200;
        e.moved = (dx > 0 ? dx : -dx) + (dy > 0 ? dy : -dy);
        if (dx > 0 && (dx >= -dy && dx >= dy)) e.swipe = SWIPE_RIGHT;
        else if (dx < 0 && (-dx >= dy && -dx >= -dy)) e.swipe = SWIPE_LEFT;
        else if (dy > 0) e.swipe = SWIPE_DOWN;
        else e.swipe = SWIPE_UP;
        inj_push(&e);
        return 0;
    }
    if (!strcmp(what, "long") && n >= 3) {
        e.type = UI_EV_LONG; e.x = e.x0 = a; e.y = e.y0 = b; e.dur_ms = 800;
        inj_push(&e);
        return 0;
    }
    return -1;
}

static void ctrl_poll(void)
{
    if (ctrl_fd < 0) return;
    for (;;) {
        char tmp[128];
        ssize_t n = read(ctrl_fd, tmp, sizeof(tmp));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            char ch = tmp[i];
            if (ch == '\n' || ch == '\r') {
                ctrl_buf[ctrl_len] = 0;
                if (ctrl_len > 0 && input_inject_cmd(ctrl_buf) != 0)
                    fprintf(stderr, "input: 无法识别的调试命令 '%s'\n", ctrl_buf);
                ctrl_len = 0;
            } else if (ctrl_len < (int)sizeof(ctrl_buf) - 1) {
                ctrl_buf[ctrl_len++] = ch;
            }
        }
    }
}

/* ------------ 事件合成 ------------ */
static void fill_up(UiEvent *e, int x, int y, int dur)
{
    e->type = UI_EV_UP;
    e->x = x; e->y = y;
    e->dur_ms = dur;
    e->moved = moved;
    int dx = x - sx, dy = y - sy;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    e->swipe = SWIPE_NONE;
    e->tap = 0;
    int near = (adx < SWIPE_MIN && ady < SWIPE_MIN);
    int near2 = (adx < TAP_MOVE_MAX && ady < TAP_MOVE_MAX);
    if (dur < TAP_TIME_MAX && (near || (dur >= TAP_SLOW_MS && near2))) {
        e->tap = 1;                       /* 点击：原地按下抬起（快慢都算） */
    } else if (!near2) {
        if (adx >= ady) e->swipe = dx > 0 ? SWIPE_RIGHT : SWIPE_LEFT;
        else            e->swipe = dy > 0 ? SWIPE_DOWN : SWIPE_UP;
    } else if (dur >= TAP_TIME_MAX) {
        e->tap = 1;                       /* 按太久但没怎么动：仍然当点击处理 */
    }
    e->x0 = sx; e->y0 = sy;
    /*
     * 点击：坐标一律回报"手指落下的位置"。
     * 真手指按下去常会滑 20~70px（电容屏还会抖），抬起位置可能已经滑出按钮，
     * 上层若按抬起位置做命中判定就会"点不动"。滑动/拖动仍然用当前位置。
     */
    if (e->tap) { e->x = sx; e->y = sy; }
    if (touch_debug)
        fprintf(stderr, "touch: 手势 起点(%d,%d) 终点(%d,%d) 位移(%d,%d) 时长%dms -> %s\n",
                e->x0, e->y0, e->x, e->y, adx, ady, dur,
                e->tap ? "点击" : (e->swipe == SWIPE_LEFT ? "左滑" :
                e->swipe == SWIPE_RIGHT ? "右滑" : e->swipe == SWIPE_UP ? "上滑" :
                e->swipe == SWIPE_DOWN ? "下滑" : "忽略"));
}

int input_poll(UiEvent *out, int timeout_ms)
{
    if (!out) return -1;

    /* 1. 先吐注入的事件 */
    if (inj_head != inj_tail) {
        *out = inj_q[inj_head];
        inj_head = (inj_head + 1) % (int)(sizeof(inj_q) / sizeof(inj_q[0]));
        return 1;
    }
    if (debug_on) ctrl_poll();
    if (inj_head != inj_tail) {
        *out = inj_q[inj_head];
        inj_head = (inj_head + 1) % (int)(sizeof(inj_q) / sizeof(inj_q[0]));
        return 1;
    }

    if (fd < 0) {
        if (timeout_ms > 0) usleep(timeout_ms * 1000);
        return 0;
    }

    /* 2. 读触摸设备（最多攒一小会儿，保证一帧内的事件一起处理） */
    struct pollfd p = { fd, POLLIN, 0 };
    int pr = poll(&p, 1, timeout_ms);
    if (pr <= 0) {
        /* 兜底：个别驱动按下包不带 SYN；60ms 后还不发就用手上的坐标发出去 */
        if (down && pending_down && now_ms() - down_ms >= 60) {
            pending_down = 0; down_emitted = 1; coords_pkt = 0;
            sx = cx; sy = cy;
            out->type = UI_EV_DOWN; out->x = cx; out->y = cy;
            out->x0 = sx; out->y0 = sy; out->dur_ms = 0;
            out->swipe = SWIPE_NONE; out->tap = 0; out->moved = 0;
            return 1;
        }
        /* 超时也要检查长按 */
        if (down && !long_fired && now_ms() - down_ms >= LONG_PRESS_MS) {
            long_fired = 1;
            out->type = UI_EV_LONG; out->x = cx; out->y = cy;
            out->x0 = sx; out->y0 = sy; out->dur_ms = (int)(now_ms() - down_ms);
            out->swipe = SWIPE_NONE; out->tap = 0; out->moved = moved;
            return 1;
        }
        return 0;
    }

    struct input_event ev;
    for (int guard = 0; guard < 256; guard++) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) break;
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X || ev.code == ABS_Y) coords_pkt = 1;
            if (ev.code == ABS_X) {
                int v = map_x(ev.value);
                if (swap_xy) cy = map_y(ev.value); else cx = v;
            } else if (ev.code == ABS_Y) {
                int v = map_y(ev.value);
                if (swap_xy) cx = map_x(ev.value); else cy = v;
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value) {
                /*
                 * 只登记，不立刻上报 DOWN：等这个包的 EV_SYN 再发，那时 cx/cy
                 * 一定是本次触摸的坐标。
                 * 关键：**不要清 coords_pkt** —— gslX680 的按下包里坐标就排在
                 * BTN_TOUCH 前面（同一个包），清掉会导致 DOWN 永远发不出去：
                 * 普通点击被丢掉，而手指拖动后补发的 DOWN 会把一次滑动当成点击。
                 */
                down = 1; pending_down = 1; down_emitted = 0;
                moved = 0; long_fired = 0;
                down_ms = now_ms();
                last_mx = last_my = -1;
            } else if (down) {
                down = 0;
                if (pending_down && !down_emitted) {
                    /* 整个触摸都没等到坐标（极罕见）：把抬起位置当按下位置，
                     * 算一次点击，绝不要让 sx/sy 停留在上一次触摸的旧值上 */
                    sx = cx; sy = cy;
                }
                pending_down = 0;
                if (touch_debug)
                    fprintf(stderr, "touch: 抬起 (%d,%d) 原始范围 X:%d..%d Y:%d..%d\n",
                            cx, cy, x_min, x_max, y_min, y_max);
                fill_up(out, cx, cy, (int)(now_ms() - down_ms));
                return 1;
            }
        } else if (ev.type == EV_SYN) {
            if (down && pending_down && coords_pkt) {
                /* 本次触摸的坐标已到齐，正式上报按下 */
                pending_down = 0; down_emitted = 1; coords_pkt = 0;
                sx = cx; sy = cy;
                if (touch_debug) fprintf(stderr, "touch: 按下 (%d,%d)\n", cx, cy);
                out->type = UI_EV_DOWN; out->x = cx; out->y = cy;
                out->x0 = sx; out->y0 = sy; out->dur_ms = 0;
                out->swipe = SWIPE_NONE; out->tap = 0; out->moved = 0;
                return 1;
            }
            if (down && !pending_down) {
                int dx = cx - sx, dy = cy - sy;
                int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                if (adx > moved || ady > moved) moved = adx > ady ? adx : ady;
                /* 只有真的移动了才上报，且相邻两次位置要有变化（防抖动刷屏） */
                if (moved > MOVE_EMIT_MIN &&
                    (last_mx < 0 || abs(cx - last_mx) >= 2 || abs(cy - last_my) >= 2)) {
                    coords_pkt = 0;      /* 本包已消费 */
                    last_mx = cx; last_my = cy;
                    out->type = UI_EV_MOVE; out->x = cx; out->y = cy;
                    out->x0 = sx; out->y0 = sy;
                    out->dur_ms = (int)(now_ms() - down_ms);
                    out->swipe = SWIPE_NONE; out->tap = 0; out->moved = moved;
                    return 1;
                }
            }
            coords_pkt = 0;      /* 包边界：本包坐标不作数到下一个包 */
        }
    }
    return 0;
}
