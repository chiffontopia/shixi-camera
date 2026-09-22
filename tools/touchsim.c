/*
 * touchsim.c — 触摸模拟器（跑在开发板上，用 /dev/uinput 造一个虚拟触摸屏）
 *
 * 用途：在没有手指的情况下验证程序的真实 evdev 输入链路与坐标标定。
 * 用法：
 *   ./touchsim tap X Y            在 (X,Y) 点一下（屏幕坐标 800x480）
 *   ./touchsim swipe X0 Y0 X1 Y1  滑动
 *   ./touchsim key X Y            只按下不抬起（配合 key release）
 *   ./touchsim release X Y
 * 说明：创建的虚拟设备会出现在 /dev/input/eventN，需要让程序连到它：
 *   SHIXI_TOUCH_DEV=/dev/input/eventN ./shixi
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <sys/time.h>

#define SCR_W 800
#define SCR_H 480
#define RAW_X 1024      /* 模拟电容屏的原始坐标范围，用于验证标定 */
#define RAW_Y 600

static int ufd = -1;

static void emit(int type, int code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    /* 时间戳交给内核/驱动填；这里只关心 type/code/value。
     * 注意：不同 glibc/内码头文件对时间字段命名不同，不要直接写 ev.time。 */
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(ufd, &ev, sizeof(ev)) < 0) perror("write uinput");
}

static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }

static void move_to(int x, int y)
{
    /* 屏幕坐标 -> 原始坐标（与真实电容屏一致的换算） */
    emit(EV_ABS, ABS_X, x * RAW_X / SCR_W);
    emit(EV_ABS, ABS_Y, y * RAW_Y / SCR_H);
    syn();
}

static int setup(void)
{
    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) { perror("打开 /dev/uinput"); return -1; }
    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    ioctl(ufd, UI_SET_EVBIT, EV_ABS);
    ioctl(ufd, UI_SET_EVBIT, EV_SYN);
    ioctl(ufd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(ufd, UI_SET_ABSBIT, ABS_X);
    ioctl(ufd, UI_SET_ABSBIT, ABS_Y);

    struct uinput_user_dev udev;
    memset(&udev, 0, sizeof(udev));
    snprintf(udev.name, UINPUT_MAX_NAME_SIZE, "shixi-touchsim");
    udev.id.bustype = BUS_VIRTUAL;
    udev.id.vendor = 0x1234;
    udev.id.product = 0x5678;
    udev.id.version = 1;
    udev.absmin[ABS_X] = 0; udev.absmax[ABS_X] = RAW_X;
    udev.absmin[ABS_Y] = 0; udev.absmax[ABS_Y] = RAW_Y;
    if (write(ufd, &udev, sizeof(udev)) < 0) { perror("write uinput_user_dev"); return -1; }
    if (ioctl(ufd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); return -1; }
    usleep(300000);
    printf("虚拟触摸设备已创建（90 秒后自动销毁）\n");
    return 0;
}

static void teardown(void)
{
    if (ufd >= 0) {
        ioctl(ufd, UI_DEV_DESTROY);
        close(ufd);
    }
}

/* serve 模式：常驻并读取 /tmp/touchsim.fifo 里的命令，方便连续测试 */
static void serve(void)
{
    const char *fifo = "/tmp/touchsim.fifo";
    unlink(fifo);
    if (mkfifo(fifo, 0666) != 0 && errno != EEXIST) { perror("mkfifo"); return; }
    printf("虚拟触摸设备就绪，命令管道: %s\n", fifo);
    printf("示例: echo 'tap 400 400' > %s\n", fifo);
    fflush(stdout);
    for (;;) {
        int fd = open(fifo, O_RDONLY);      /* 阻塞等待写入方 */
        if (fd < 0) { usleep(200000); continue; }
        char buf[256];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        char *nl;
        while ((nl = strchr(buf, '\n')) != NULL) *nl = ' ';
        char what[16] = {0};
        int a = 0, b = 0, c = 0, d = 0;
        int cnt = sscanf(buf, "%15s %d %d %d %d", what, &a, &b, &c, &d);
        if (cnt >= 3 && !strcmp(what, "tap")) {
            move_to(a, b);
            emit(EV_KEY, BTN_TOUCH, 1); syn();
            usleep(80000);
            emit(EV_KEY, BTN_TOUCH, 0); syn();
            printf("tap %d %d\n", a, b);
        } else if (cnt >= 5 && !strcmp(what, "swipe")) {
            move_to(a, b);
            emit(EV_KEY, BTN_TOUCH, 1); syn();
            for (int i = 1; i <= 12; i++) {
                move_to(a + (c - a) * i / 12, b + (d - b) * i / 12);
                usleep(18000);
            }
            emit(EV_KEY, BTN_TOUCH, 0); syn();
            printf("swipe %d %d -> %d %d\n", a, b, c, d);
        } else if (cnt >= 3 && (!strcmp(what, "jit") || !strcmp(what, "slow"))) {
            int slow = !strcmp(what, "slow");
            int steps = slow ? 18 : 6, wait_ms = slow ? 50 : 40, jit = slow ? 9 : 7;
            move_to(a, b);
            emit(EV_KEY, BTN_TOUCH, 1); syn();
            for (int i = 1; i <= steps; i++) {
                int dx = ((i * 37) % (2 * jit + 1)) - jit;
                int dy = ((i * 53) % (2 * jit + 1)) - jit;
                move_to(a + dx, b + dy);
                usleep(wait_ms * 1000);
            }
            move_to(a, b + 1);
            emit(EV_KEY, BTN_TOUCH, 0); syn();
            printf("%s %d %d\n", slow ? "slow" : "jit", a, b);
        } else if (cnt >= 3 && !strcmp(what, "drift")) {
            /* drift X Y DX DY [ms]：按住期间从 (X,Y) 单调漂到 (X+DX,Y+DY) 再抬起 */
            int dx = cnt >= 5 ? c : 70;              /* 屏幕像素 */
            int dy = cnt >= 6 ? d : 10;
            int total = 300, steps = 6;
            int sx_ = a * 1024 / 800, sy_ = b * 600 / 480;
            int ex_ = (a + dx) * 1024 / 800, ey_ = (b + dy) * 600 / 480;
            emit(EV_ABS, ABS_X, sx_); emit(EV_ABS, ABS_Y, sy_); syn();
            emit(EV_KEY, BTN_TOUCH, 1); syn();
            for (int i = 1; i <= steps; i++) {
                emit(EV_ABS, ABS_X, sx_ + (ex_ - sx_) * i / steps);
                emit(EV_ABS, ABS_Y, sy_ + (ey_ - sy_) * i / steps);
                syn();
                usleep(total / steps * 1000);
            }
            emit(EV_KEY, BTN_TOUCH, 0); syn();
            printf("drift %d %d -> %d %d（%dms）\n", a, b, a + dx, b + dy, total);
        } else if (cnt >= 3 && !strcmp(what, "tapafter")) {
            /* 模拟"坐标排在 BTN_TOUCH 之后"的面板：先报按下，再报坐标。
             * 用来验证输入层是否会用到上一次的旧坐标（旧实现就会点错地方）。 */
            emit(EV_KEY, BTN_TOUCH, 1); syn();
            usleep(15000);
            move_to(a, b);
            usleep(60000);
            emit(EV_KEY, BTN_TOUCH, 0); syn();
            printf("tapafter %d %d（坐标后到）\n", a, b);
        } else if (cnt >= 3 && !strcmp(what, "down")) {
            move_to(a, b);
            emit(EV_KEY, BTN_TOUCH, 1); syn();
            printf("down %d %d\n", a, b);
        } else if (cnt >= 3 && !strcmp(what, "move")) {
            move_to(a, b);
            printf("move %d %d\n", a, b);
        } else if (cnt >= 3 && !strcmp(what, "up")) {
            move_to(a, b);
            emit(EV_KEY, BTN_TOUCH, 0); syn();
            printf("up %d %d\n", a, b);
        } else if (!strcmp(what, "quit")) {
            return;
        } else {
            printf("未知命令: %s\n", buf);
        }
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s tap X Y | swipe X0 Y0 X1 Y1 | key X Y | release X Y | serve\n", argv[0]);
        return 1;
    }
    if (setup() != 0) return 1;

    if (!strcmp(argv[1], "serve")) {
        serve();
        teardown();
        return 0;
    }

    const char *cmd = argv[1];
    if (!strcmp(cmd, "tap") && argc >= 4) {
        int x = atoi(argv[2]), y = atoi(argv[3]);
        move_to(x, y);
        emit(EV_KEY, BTN_TOUCH, 1); syn();
        usleep(90000);
        emit(EV_KEY, BTN_TOUCH, 0); syn();
        printf("tap %d %d\n", x, y);
    } else if (!strcmp(cmd, "swipe") && argc >= 6) {
        int x0 = atoi(argv[2]), y0 = atoi(argv[3]), x1 = atoi(argv[4]), y1 = atoi(argv[5]);
        move_to(x0, y0);
        emit(EV_KEY, BTN_TOUCH, 1); syn();
        for (int i = 1; i <= 12; i++) {
            move_to(x0 + (x1 - x0) * i / 12, y0 + (y1 - y0) * i / 12);
            usleep(18000);
        }
        emit(EV_KEY, BTN_TOUCH, 0); syn();
        printf("swipe %d %d -> %d %d\n", x0, y0, x1, y1);
    } else if ((!strcmp(cmd, "jit") || !strcmp(cmd, "slow")) && argc >= 4) {
        /* 可选参数：jit X Y [抖动幅度(原始值)] [总时长ms] */
        /*
         * 模拟真手指：按下时不稳、坐标一直在抖。
         *   jit  X Y  快速点一下（约 250ms，水平漂移 ~60 原始值）
         *   slow X Y  慢慢按（约 900ms，漂移更大）
         * 用来验证"电容屏抖动导致点击被误判成滑动"这类问题。
         */
        int x = atoi(argv[2]), y = atoi(argv[3]);
        int slow = !strcmp(cmd, "slow");
        int jit = argc > 4 ? atoi(argv[4]) : (slow ? 60 : 70);   /* 原始坐标抖动幅度 */
        int total = argc > 5 ? atoi(argv[5]) : (slow ? 900 : 300);
        int steps = slow ? 18 : 6;
        int wait_ms = total / steps;
        move_to(x, y);
        emit(EV_KEY, BTN_TOUCH, 1); syn();
        for (int i = 1; i <= steps; i++) {
            /* 抖动：交替偏一点，最后回到原点附近（真手指就是这样） */
            int dx = ((i * 37) % (2 * jit + 1)) - jit;
            int dy = ((i * 53) % (2 * jit + 1)) - jit;
            move_to(x + dx, y + dy);
            usleep(wait_ms * 1000);
        }
        move_to(x, y + 1);
        emit(EV_KEY, BTN_TOUCH, 0); syn();
        printf("%s %d %d（%d 步，抖动 ±%d 原始值，共约 %d ms）\n",
               slow ? "慢按" : "带抖动点击", x, y, steps, jit, steps * wait_ms);
    } else if (!strcmp(cmd, "key") && argc >= 4) {
        move_to(atoi(argv[2]), atoi(argv[3]));
        emit(EV_KEY, BTN_TOUCH, 1); syn();
        printf("按下 %s %s\n", argv[2], argv[3]);
        printf("设备保持存在 90 秒，可继续用 key/release 操作；Ctrl+C 结束\n");
        sleep(90);
    } else if (!strcmp(cmd, "release") && argc >= 4) {
        move_to(atoi(argv[2]), atoi(argv[3]));
        emit(EV_KEY, BTN_TOUCH, 0); syn();
        printf("抬起\n");
        usleep(200000);
    } else {
        fprintf(stderr, "参数不对\n");
        teardown();
        return 1;
    }
    usleep(200000);
    teardown();
    return 0;
}
