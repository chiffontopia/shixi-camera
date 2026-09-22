/*
 * input.h — 触摸屏输入（Linux evdev）
 *
 * 说明：
 *   - 设备默认 /dev/input/event0（GEC6818 为 gslX680 电容屏，ABS_X 0..1024 / ABS_Y 0..600），
 *     坐标范围通过 EVIOCGABS 读取后自动映射到 800x480，可用环境变量覆盖：
 *       SHIXI_TOUCH_DEV=/dev/input/eventN   指定设备
 *       SHIXI_TOUCH_SWAP=1                  XY 交换
 *       SHIXI_TOUCH_INVERT_X=1 / INVERT_Y=1 反向
 *   - 事件是"语义化"的：按下/移动/抬起，抬起时附带 {点击 | 滑动方向 | 长按}
 *   - 调试通道：/tmp/shixi_ctrl 命名管道，可注入 tap/swipe/down/up/long，
 *     方便无人值守时自动演示（见 tools/touchsim.c 与 README）
 */
#ifndef SHIXI_INPUT_H
#define SHIXI_INPUT_H

typedef enum {
    UI_EV_NONE = 0,
    UI_EV_DOWN,      /* 手指按下 */
    UI_EV_MOVE,      /* 拖动中 */
    UI_EV_LONG,      /* 长按（按下超过阈值时长后触发一次，此时仍未抬起） */
    UI_EV_UP         /* 抬起（结合 tap/swipe 字段判断手势） */
} UiEvType;

typedef enum {
    SWIPE_NONE = 0,
    SWIPE_LEFT,
    SWIPE_RIGHT,
    SWIPE_UP,
    SWIPE_DOWN
} SwipeDir;

typedef struct {
    UiEvType type;
    int  x, y;       /* 当前坐标 */
    int  x0, y0;     /* 按下时的坐标 */
    int  dur_ms;     /* 按下持续时长（UP/LONG 有效） */
    int  swipe;      /* SwipeDir（UP 有效） */
    int  tap;        /* 1 = 本次 UP 属于点击 */
    int  moved;      /* 按下后累计移动距离 */
} UiEvent;

int  input_init(void);
/* 取事件；timeout_ms<0 表示阻塞等待，返回 1 有事件，0 超时，-1 出错 */
int  input_poll(UiEvent *out, int timeout_ms);
int  input_ready(void);          /* 触摸设备是否可用 */
const char *input_device_name(void);
/* 调试注入（返回 0 成功）：cmd 形如 "tap 400 430" / "swipe 700 240 100 240" */
int  input_inject_cmd(const char *cmd);
void input_debug_enable(int on);

#endif /* SHIXI_INPUT_H */
