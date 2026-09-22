/*
 * camera.c — V4L2 采集 + 演示信号源实现
 */
#include "camera.h"
#include "image.h"
#include "font.h"
#include "util.h"
#include "v4l2compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <linux/videodev2.h>

#define MAX_BUFS 4
#define JPEG_CAP (600 * 1024)

typedef struct {
    void  *start;
    size_t length;
} V4LBuf;

/* 双缓冲预览：写线程写 buf[wr]，界面线程读 buf[rd] */
typedef struct {
    Surface *rgb;
    unsigned char *jpeg;      /* 最近一帧的 JPEG（可能是摄像头原始数据或编码结果） */
    int  jpeg_len;
    uint64_t seq;
} FrameSlot;

static struct {
    int   running;
    int   inited;
    pthread_t th;
    pthread_mutex_t lock;

    CamSource src;
    char  dev_path[64];
    int   fd;
    V4LBuf bufs[MAX_BUFS];
    int   nbufs;
    uint32_t pixfmt;
    int   width, height;

    FrameSlot slot[2];
    volatile int wr;          /* 写入槽 */
    volatile int rd;          /* 最近完成的槽（界面读这个） */
    volatile int pending_event;   /* CamEvent，界面取走后清零 */
    int  got_first_frame;         /* 是否已经收到过帧（决定看门狗宽严） */
    uint64_t open_ms;             /* 进入 V4L2 模式的时间（判断"无信号"用） */
    int  req_fps;                 /* 期望帧率（0=默认，不主动设置） */
    int  old_abi;                 /* 1 = 内核 3.4 的老 v4l2_buffer ABI */
    int  in_v4l2;                 /* 当前是否处于 V4L2 模式（无信号判断用） */

    /* 最近一帧 JPEG（供存照片/录像使用；部分信号源并非每帧都产出 JPEG） */
    unsigned char *last_jpeg;
    int  last_jpeg_len;
    uint64_t last_jpeg_ms;
    uint64_t last_frame_ms2;      /* 最近一次成功出帧的时间 */
    uint64_t seq;
    int   fps;
    int   frame_count;
    uint64_t fps_t0;
    int   fail_count;
} cam;

/* ================= 工具：YUYV -> RGB ================= */
static void yuyv_to_rgb(const unsigned char *yuyv, Surface *s)
{
    int w = s->w, h = s->h;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = yuyv + (size_t)y * w * 2;
        uint32_t *dst = s->px + (size_t)y * s->stride;
        for (int x = 0; x < w; x += 2) {
            int y0 = row[0], u = row[1], y1 = row[2], v = row[3];
            row += 4;
            int c0 = y0 - 16, c1 = y1 - 16, d = u - 128, e = v - 128;
            if (c0 < 0) c0 = 0;
            if (c1 < 0) c1 = 0;
            int r0 = (298 * c0 + 409 * e + 128) >> 8;
            int g0 = (298 * c0 - 100 * d - 208 * e + 128) >> 8;
            int b0 = (298 * c0 + 516 * d + 128) >> 8;
            int r1 = (298 * c1 + 409 * e + 128) >> 8;
            int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
            int b1 = (298 * c1 + 516 * d + 128) >> 8;
            r0 = r0 < 0 ? 0 : (r0 > 255 ? 255 : r0);
            g0 = g0 < 0 ? 0 : (g0 > 255 ? 255 : g0);
            b0 = b0 < 0 ? 0 : (b0 > 255 ? 255 : b0);
            r1 = r1 < 0 ? 0 : (r1 > 255 ? 255 : r1);
            g1 = g1 < 0 ? 0 : (g1 > 255 ? 255 : g1);
            b1 = b1 < 0 ? 0 : (b1 > 255 ? 255 : b1);
            dst[x]     = ((uint32_t)r0 << 16) | ((uint32_t)g0 << 8) | (uint32_t)b0;
            dst[x + 1] = ((uint32_t)r1 << 16) | ((uint32_t)g1 << 8) | (uint32_t)b1;
        }
    }
}

/* ================= 工具：JPEG -> RGB ================= */
static int jpeg_to_rgb(const unsigned char *jpeg, int len, Surface *s)
{
    Surface *tmp = image_load_mem(jpeg, len);
    if (!tmp) return -1;
    /* 摄像头分辨率可能不是 640x480，这里做一次 cover 缩放 */
    if (tmp->w == s->w && tmp->h == s->h) {
        for (int y = 0; y < s->h; y++)
            memcpy(s->px + (size_t)y * s->stride, tmp->px + (size_t)y * tmp->stride, (size_t)s->w * 4);
    } else {
        Surface *sc = image_thumbnail(tmp, s->w, s->h);
        if (sc) {
            for (int y = 0; y < s->h; y++)
                memcpy(s->px + (size_t)y * s->stride, sc->px + (size_t)y * sc->stride, (size_t)s->w * 4);
            gfx_surface_free(sc);
        }
    }
    gfx_surface_free(tmp);
    return 0;
}

/* ================= 演示信号源 ================= */
static void testpattern_render(Surface *s, uint64_t t)
{
    int w = s->w, h = s->h;
    static int phase = 0;
    phase++;
    /* 天空渐变 + 太阳 + 移动云 + 地面 + 网格 + 文字标注 + 计时 */
    for (int y = 0; y < h; y++) {
        uint32_t c;
        if (y < h * 3 / 5) {
            c = gfx_blend(RGB(0x12, 0x2a, 0x4a), RGB(0x3f, 0x86, 0xc8), y * 255 / (h * 3 / 5));
        } else {
            int yy = y - h * 3 / 5;
            c = gfx_blend(RGB(0x1d, 0x3b, 0x2a), RGB(0x0c, 0x1a, 0x14), yy * 255 / (h - h * 3 / 5));
        }
        gfx_fill_rect(s, 0, y, w, 1, c);
    }
    /* 太阳 */
    int sx = w * 3 / 4, sy = h / 5;
    for (int r = 60; r > 0; r -= 4)
        gfx_fill_circle_a(s, sx, sy, r, RGB(0xff, 0xe0, 0x8a), 26);
    gfx_fill_circle(s, sx, sy, 34, RGB(0xff, 0xf3, 0xc4));
    /* 云：随时间平移 */
    for (int k = 0; k < 3; k++) {
        int cx = (int)((t / 40 + k * 260) % (w + 200)) - 100;
        int cy = 60 + k * 42;
        gfx_fill_circle_a(s, cx, cy, 30, RGB(0xff, 0xff, 0xff), 210);
        gfx_fill_circle_a(s, cx + 34, cy + 8, 24, RGB(0xff, 0xff, 0xff), 190);
        gfx_fill_circle_a(s, cx - 32, cy + 10, 20, RGB(0xff, 0xff, 0xff), 180);
    }
    /* 地面网格（透视） */
    for (int i = 0; i <= 12; i++) {
        int x = (i - 6) * (w / 6) / 2 + w / 2;
        gfx_line_a(s, w / 2 + (x - w / 2) / 6, h * 3 / 5, x, h, RGB(0x4a, 0xd6, 0xa0), 90);
    }
    for (int i = 1; i < 8; i++) {
        int y = h * 3 / 5 + (h - h * 3 / 5) * i * i / 64;
        gfx_line_a(s, 0, y, w, y, RGB(0x4a, 0xd6, 0xa0), 70);
    }
    /* 中央十字准星 + 文字 */
    gfx_line_a(s, w / 2 - 18, h / 2, w / 2 + 18, h / 2, RGB(0xff, 0xff, 0xff), 150);
    gfx_line_a(s, w / 2, h / 2 - 18, w / 2, h / 2 + 18, RGB(0xff, 0xff, 0xff), 150);
    gfx_fill_round_rect_a(s, w / 2 - 130, h - 74, 260, 40, 10, RGB(0, 0, 0), 120);
    text_draw_vcenter_center(s, w / 2, h - 74, 40, "演示画面 · 未检测到摄像头", FONT_BODY, RGB(0xff, 0xff, 0xff));
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d",
             (int)((t / 1000 / 3600) % 100), (int)((t / 1000 / 60) % 60), (int)((t / 1000) % 60));
    gfx_fill_round_rect_a(s, 16, h - 74, 150, 40, 10, RGB(0, 0, 0), 120);
    text_draw_vcenter_center(s, 16 + 75, h - 74, 40, tbuf, FONT_BODY, RGB(0x9f, 0xff, 0xd8));
    /* 右下角运动的方块，证明画面是活的 */
    int bx = 40 + (int)((t / 16) % (w - 120));
    gfx_fill_round_rect(s, bx, h - 150, 60, 60, 12, RGB(0xff, 0x6b, 0x6b));
    gfx_fill_round_rect(s, w - bx - 100, 120, 40, 40, 10, RGB(0x6b, 0xc4, 0xff));
    (void)phase;
}

/* ================= V4L2 ================= */
static int v4l2_try_format(int fd, int w, int h)
{
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) == 0 && fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG)
        return V4L2_PIX_FMT_MJPEG;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) == 0 && fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
        return V4L2_PIX_FMT_YUYV;
    return 0;
}

static int v4l2_open_device(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) { close(fd); return -1; }
    uint32_t caps = v4l2_device_caps(&cap);
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        close(fd);
        return -1;
    }
    /* 至少要支持 MJPEG 或 YUYV */
    struct v4l2_fmtdesc f;
    int ok = 0;
    for (int i = 0; i < 16; i++) {
        memset(&f, 0, sizeof(f));
        f.index = i;
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &f) != 0) break;
        if (f.pixelformat == V4L2_PIX_FMT_MJPEG || f.pixelformat == V4L2_PIX_FMT_YUYV) ok = 1;
    }
    if (!ok) { close(fd); return -1; }
    return fd;
}

/* 扫描可用摄像头：优先设备名含 "USB" 的，其次第一个可用的 */
static int v4l2_find_camera(char *path_out, int n)
{
    char best[64] = {0};
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        int fd = v4l2_open_device(path);
        if (fd < 0) continue;
        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        ioctl(fd, VIDIOC_QUERYCAP, &cap);
        close(fd);
        if (strstr((char *)cap.bus_info, "usb") || strstr((char *)cap.driver, "uvc")) {
            snprintf(best, sizeof(best), "%s", path);
            break;
        }
        if (!best[0]) snprintf(best, sizeof(best), "%s", path);
    }
    if (!best[0]) return -1;
    snprintf(path_out, n, "%s", best);
    return 0;
}

static int v4l2_start(void)
{
    cam.fd = v4l2_open_device(cam.dev_path);
    if (cam.fd < 0) return -1;

    cam.old_abi = v4l2_detect_old_abi(cam.fd);
    if (cam.old_abi)
        fprintf(stderr, "camera: 使用内核 3.4 的老 v4l2_buffer ABI（68 字节）\n");

    uint32_t fmt = v4l2_try_format(cam.fd, cam.width, cam.height);
    if (!fmt) { close(cam.fd); cam.fd = -1; return -1; }
    cam.pixfmt = fmt;

    /* 读回实际协商到的分辨率 */
    struct v4l2_format gf;
    memset(&gf, 0, sizeof(gf));
    gf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam.fd, VIDIOC_G_FMT, &gf) == 0) {
        if (gf.fmt.pix.width > 0) cam.width = gf.fmt.pix.width;
        if (gf.fmt.pix.height > 0) cam.height = gf.fmt.pix.height;
    }

    /* 可选：设置帧率（部分摄像头在低帧率下功耗更低、更稳定） */
    if (cam.req_fps > 0) {
        struct v4l2_streamparm sp;
        memset(&sp, 0, sizeof(sp));
        sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(cam.fd, VIDIOC_G_PARM, &sp) == 0) {
            sp.parm.capture.timeperframe.numerator = 1;
            sp.parm.capture.timeperframe.denominator = (uint32_t)cam.req_fps;
            if (ioctl(cam.fd, VIDIOC_S_PARM, &sp) == 0)
                fprintf(stderr, "camera: 帧率设置为 %u fps\n",
                        sp.parm.capture.timeperframe.denominator);
        }
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = MAX_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(cam.fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 2) {
        close(cam.fd); cam.fd = -1; return -1;
    }
    cam.nbufs = req.count;
    for (int i = 0; i < cam.nbufs; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (v4l2_ioctl_querybuf(cam.fd, &b, cam.old_abi) != 0) { cam.nbufs = i; break; }
        cam.bufs[i].length = b.length;
        cam.bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam.fd, b.m.offset);
        if (cam.bufs[i].start == MAP_FAILED) { cam.bufs[i].start = NULL; cam.nbufs = i; break; }
    }
    for (int i = 0; i < cam.nbufs; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (v4l2_ioctl_qbuf(cam.fd, &b, cam.old_abi) != 0) break;
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam.fd, VIDIOC_STREAMON, &type) != 0) {
        for (int i = 0; i < cam.nbufs; i++)
            if (cam.bufs[i].start) munmap(cam.bufs[i].start, cam.bufs[i].length);
        close(cam.fd); cam.fd = -1; return -1;
    }
    cam.open_ms = now_ms();
    cam.in_v4l2 = 1;
    cam.got_first_frame = 0;
    cam.last_frame_ms2 = 0;
    fprintf(stderr, "camera: %s %dx%d %s\n", cam.dev_path, cam.width, cam.height,
            cam.pixfmt == V4L2_PIX_FMT_MJPEG ? "MJPEG" : "YUYV");
    return 0;
}

static void v4l2_stop(void)
{
    cam.in_v4l2 = 0;
    if (cam.fd < 0) return;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cam.fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < cam.nbufs; i++)
        if (cam.bufs[i].start) munmap(cam.bufs[i].start, cam.bufs[i].length);
    cam.nbufs = 0;
    close(cam.fd);
    cam.fd = -1;
}

/*
 * 取一帧：返回缓冲区下标（>=0）或 -1。
 * 注意：缓冲区在调用 v4l2_release() 之前一直处于"已出队"状态，
 *      驱动不会再往里写数据，避免处理期间被新帧覆盖。
 */
static int v4l2_grab(unsigned char **data, int *len)
{
    /* 先用 poll 等一下，避免 DQBUF 永久阻塞导致线程无法退出 */
    struct pollfd p = { cam.fd, POLLIN, 0 };
    int pr = poll(&p, 1, 500);
    if (pr < 0) return -2;                       /* 出错（含设备被拔） */
    if (pr == 0) return -1;                      /* 超时，可能只是没出帧 */
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -2;   /* 设备已掉线 */


    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (v4l2_ioctl_dqbuf(cam.fd, &b, cam.old_abi) != 0) {
        return errno == ENODEV ? -2 : -1;
    }
    if (b.index >= (unsigned)cam.nbufs) return -1;
    *data = (unsigned char *)cam.bufs[b.index].start;
    *len = b.bytesused;
    return (int)b.index;
}

static void v4l2_release(int idx)
{
    if (idx < 0) return;
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = (unsigned)idx;
    v4l2_ioctl_qbuf(cam.fd, &b, cam.old_abi);
}

/* ================= 采集线程 ================= */
static void publish(Surface *rgb, const unsigned char *jpeg, int jpeg_len)
{
    if (jpeg && jpeg_len > 0 && jpeg_len <= JPEG_CAP) {
        if (cam.last_jpeg) {
            memcpy(cam.last_jpeg, jpeg, jpeg_len);
            cam.last_jpeg_len = jpeg_len;
            cam.last_jpeg_ms = now_ms();
        }
    }
    int slot = cam.wr;
    /* 写另一块，避免界面正在读的那块被覆盖 */
    if (slot == cam.rd) slot ^= 1;
    FrameSlot *fs = &cam.slot[slot];
    if (rgb) {
        for (int y = 0; y < rgb->h; y++)
            memcpy(fs->rgb->px + (size_t)y * fs->rgb->stride,
                   rgb->px + (size_t)y * rgb->stride, (size_t)rgb->w * 4);
    }
    if (jpeg && jpeg_len > 0 && jpeg_len <= JPEG_CAP) {
        memcpy(fs->jpeg, jpeg, jpeg_len);
        fs->jpeg_len = jpeg_len;
    }
    fs->seq = ++cam.seq;
    cam.last_frame_ms2 = now_ms();
    cam.got_first_frame = 1;
    cam.rd = slot;
    cam.wr = slot ^ 1;
    cam.frame_count++;
}

static void *capture_thread(void *arg)
{
    (void)arg;
    unsigned char *inbuf = (unsigned char *)malloc(JPEG_CAP);
    Surface *work = gfx_surface_new(cam.width, cam.height);
    if (!inbuf || !work) {
        free(inbuf); gfx_surface_free(work);
        return NULL;
    }
    int enc_quality = 82;

    while (cam.running) {
        uint64_t t0 = now_ms();

        if (cam.src == CAM_SRC_V4L2) {
            unsigned char *raw = NULL;
            int rawlen = 0;
            int buf_idx = v4l2_grab(&raw, &rawlen);
            if (buf_idx == -2) cam.fail_count += 100;      /* 硬错误：设备掉线 */
            else if (buf_idx < 0 || rawlen <= 0) cam.fail_count++;
            else cam.fail_count = 0;

            /*
             * 看门狗：V4L2 模式下超过 3 秒没有出帧（拔线、供电不足掉线、
             * 驱动没起来等），自动退回演示画面，避免界面永远停在"正在启动摄像头"。
             */
            uint64_t idle_ms = cam.last_frame_ms2 ? (now_ms() - cam.last_frame_ms2)
                                                  : (now_ms() - cam.open_ms);
            uint64_t timeout_ms = cam.got_first_frame ? 2500 : 8000;
            if (cam.fail_count > 0 && idle_ms > timeout_ms) {
                fprintf(stderr, "camera: %s %llu 秒无帧（设备掉线或供电不足），退回演示画面\n",
                        cam.dev_path, (unsigned long long)(idle_ms / 1000));
                v4l2_stop();
                cam.src = CAM_SRC_TESTPATTERN;
                snprintf(cam.dev_path, sizeof(cam.dev_path), "-");
                cam.fps = 0;
                cam.frame_count = 0;
                cam.fps_t0 = 0;
                cam.fail_count = 0;
                cam.pending_event = CAM_EV_FELL_BACK;
                continue;
            }
            if (buf_idx < 0 || rawlen <= 0) {
                usleep(20000);
                continue;
            }
            if (cam.pixfmt == V4L2_PIX_FMT_MJPEG) {
                if (jpeg_to_rgb(raw, rawlen, work) == 0 && rawlen <= JPEG_CAP) {
                    pthread_mutex_lock(&cam.lock);
                    publish(work, raw, rawlen);
                    pthread_mutex_unlock(&cam.lock);
                }
            } else { /* YUYV：转 RGB，并编码成 JPEG 供拍照/录像 */
                yuyv_to_rgb(raw, work);
                int jlen = 0;
                unsigned char *jbuf = NULL;
                if (image_encode_jpeg_mem(work, enc_quality, &jbuf, &jlen) == 0) {
                    pthread_mutex_lock(&cam.lock);
                    publish(work, jbuf, jlen);
                    pthread_mutex_unlock(&cam.lock);
                    free(jbuf);
                } else {
                    pthread_mutex_lock(&cam.lock);
                    publish(work, NULL, 0);
                    pthread_mutex_unlock(&cam.lock);
                }
            }
            v4l2_release(buf_idx);      /* 处理完再还给驱动 */
        } else { /* 演示画面 */
            testpattern_render(work, now_ms());
            /* 演示模式的 JPEG 按需生成（每秒最多 2 帧，够用） */
            static uint64_t last_enc = 0;
            int jlen = 0;
            unsigned char *jbuf = NULL;
            if (t0 - last_enc > 100) {
                last_enc = t0;
                if (image_encode_jpeg_mem(work, enc_quality, &jbuf, &jlen) != 0) { jlen = 0; jbuf = NULL; }
            }
            pthread_mutex_lock(&cam.lock);
            publish(work, jbuf, jlen);
            pthread_mutex_unlock(&cam.lock);
            if (jbuf) free(jbuf);
            usleep(50000);   /* 演示模式 ~20fps */
        }

        /* 帧率统计 */
        uint64_t now = now_ms();
        if (cam.fps_t0 == 0) cam.fps_t0 = now;
        if (now - cam.fps_t0 >= 1000) {
            cam.fps = (int)(cam.frame_count * 1000 / (now - cam.fps_t0));
            cam.frame_count = 0;
            cam.fps_t0 = now;
        }
    }
    free(inbuf);
    gfx_surface_free(work);
    return NULL;
}

/* ================= 对外接口 ================= */
int camera_open(void)
{
    if (cam.inited) return 0;
    memset(&cam, 0, sizeof(cam));
    cam.fd = -1;
    cam.width = CAM_FRAME_W;
    cam.height = CAM_FRAME_H;
    /* 供电吃紧的摄像头可以降分辨率/降帧率：SHIXI_CAM_SIZE=320x240 SHIXI_CAM_FPS=10 */
    const char *sz = getenv("SHIXI_CAM_SIZE");
    if (sz) {
        int w = 0, h = 0;
        if (sscanf(sz, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 1920 && h <= 1080) {
            cam.width = w;
            cam.height = h;
        }
    }
    const char *fp = getenv("SHIXI_CAM_FPS");
    if (fp) {
        int f = atoi(fp);
        if (f >= 1 && f <= 30) cam.req_fps = f;
    }
    pthread_mutex_init(&cam.lock, NULL);

    cam.last_jpeg = (unsigned char *)malloc(JPEG_CAP);
    if (!cam.last_jpeg) return -1;
    for (int i = 0; i < 2; i++) {
        cam.slot[i].rgb = gfx_surface_new(cam.width, cam.height);
        cam.slot[i].jpeg = (unsigned char *)malloc(JPEG_CAP);
        cam.slot[i].jpeg_len = 0;
        cam.slot[i].seq = 0;
        if (!cam.slot[i].rgb || !cam.slot[i].jpeg) return -1;
    }

    /* 环境变量可强制指定设备；否则自动扫描 */
    const char *force = getenv("SHIXI_CAMERA");
    if (force && !strcmp(force, "none")) {
        cam.src = CAM_SRC_TESTPATTERN;
    } else if (force && *force) {
        snprintf(cam.dev_path, sizeof(cam.dev_path), "%s", force);
        cam.src = CAM_SRC_V4L2;
    } else {
        char path[64];
        if (v4l2_find_camera(path, sizeof(path)) == 0) {
            snprintf(cam.dev_path, sizeof(cam.dev_path), "%s", path);
            cam.src = CAM_SRC_V4L2;
        } else {
            cam.src = CAM_SRC_TESTPATTERN;
        }
    }

    if (cam.src == CAM_SRC_V4L2 && v4l2_start() != 0) {
        fprintf(stderr, "camera: %s 初始化失败，切换到演示画面\n", cam.dev_path);
        cam.src = CAM_SRC_TESTPATTERN;
        snprintf(cam.dev_path, sizeof(cam.dev_path), "-");
    }
    if (cam.src == CAM_SRC_TESTPATTERN) {
        snprintf(cam.dev_path, sizeof(cam.dev_path), "-");
        fprintf(stderr, "camera: 未检测到摄像头，使用演示画面\n");
    }

    cam.running = 1;
    if (pthread_create(&cam.th, NULL, capture_thread, NULL) != 0) {
        cam.running = 0;
        return -1;
    }
    cam.inited = 1;
    return 0;
}

/*
 * 热插拔：演示模式下定期找一次摄像头，找到就切过去。
 * 切换时先停采集线程（避免同时访问 V4L2 句柄），再重新起线程。
 */
int camera_rescan(void)
{
    if (!cam.inited) return -1;
    if (cam.src == CAM_SRC_V4L2) return 0;

    static uint64_t last_scan = 0;
    uint64_t now = now_ms();
    if (now - last_scan < 2000) return 0;     /* 2 秒扫一次，避免频繁 open 设备 */
    last_scan = now;

    char path[64];
    if (v4l2_find_camera(path, sizeof(path)) != 0) return -1;

    /* 停掉演示画面线程 */
    cam.running = 0;
    pthread_join(cam.th, NULL);

    snprintf(cam.dev_path, sizeof(cam.dev_path), "%s", path);
    cam.src = CAM_SRC_V4L2;
    cam.fps = 0;
    cam.frame_count = 0;
    cam.fps_t0 = 0;
    cam.fail_count = 0;

    if (v4l2_start() != 0) {
        fprintf(stderr, "camera: %s 打开失败，继续使用演示画面\n", cam.dev_path);
        cam.src = CAM_SRC_TESTPATTERN;
        snprintf(cam.dev_path, sizeof(cam.dev_path), "-");
        cam.running = 1;
        pthread_create(&cam.th, NULL, capture_thread, NULL);
        return -1;
    }

    cam.running = 1;
    if (pthread_create(&cam.th, NULL, capture_thread, NULL) != 0) {
        cam.running = 0;
        v4l2_stop();
        cam.src = CAM_SRC_TESTPATTERN;
        snprintf(cam.dev_path, sizeof(cam.dev_path), "-");
        return -1;
    }
    fprintf(stderr, "camera: 已检测到摄像头 %s，切换到实时画面\n", cam.dev_path);
    cam.last_jpeg_len = 0;      /* 让拍照走"重新编码当前预览帧"分支，避免拿到旧画面 */
    cam.pending_event = CAM_EV_ACQUIRED;
    return 1;
}

void camera_close(void)
{
    if (!cam.inited) return;
    cam.running = 0;
    pthread_join(cam.th, NULL);
    if (cam.src == CAM_SRC_V4L2) v4l2_stop();
    for (int i = 0; i < 2; i++) {
        if (cam.slot[i].rgb) gfx_surface_free(cam.slot[i].rgb);
        if (cam.slot[i].jpeg) free(cam.slot[i].jpeg);
        cam.slot[i].rgb = NULL;
        cam.slot[i].jpeg = NULL;
    }
    free(cam.last_jpeg);
    cam.last_jpeg = NULL;
    pthread_mutex_destroy(&cam.lock);
    cam.inited = 0;
}

int camera_is_open(void) { return cam.inited; }
CamSource camera_source(void) { return cam.src; }
const char *camera_device_path(void) { return cam.dev_path; }
int camera_fps(void) { return cam.fps; }
int camera_pixel_format(void) { return (int)cam.pixfmt; }

const Surface *camera_preview(uint64_t *seq_out)
{
    if (!cam.inited) return NULL;
    int rd = cam.rd;
    if (seq_out) *seq_out = cam.slot[rd].seq;
    if (cam.slot[rd].seq == 0) return NULL;
    return cam.slot[rd].rgb;
}

int camera_latest_jpeg(unsigned char *out, int out_cap)
{
    if (!cam.inited || !out) return -1;
    pthread_mutex_lock(&cam.lock);
    int len = cam.last_jpeg_len;
    if (len > out_cap) len = -1;
    if (len > 0) memcpy(out, cam.last_jpeg, len);
    pthread_mutex_unlock(&cam.lock);
    return len;
}

int camera_latest_jpeg_len(void)
{
    if (!cam.inited) return 0;
    return cam.last_jpeg_len;
}

int camera_no_signal_ms(void)
{
    if (!cam.inited || cam.src != CAM_SRC_V4L2) return 0;
    if (cam.last_frame_ms2) {
        uint64_t d = now_ms() - cam.last_frame_ms2;
        return (int)(d > 100000 ? 100000 : d);
    }
    uint64_t d = now_ms() - cam.open_ms;
    return (int)(d > 100000 ? 100000 : d);
}

int camera_take_event(void)
{
    int e = cam.pending_event;
    cam.pending_event = CAM_EV_NONE;
    return e;
}

/* 最近一帧 JPEG 的年龄（毫秒）；用于判断是否需要重新编码 */
int camera_jpeg_age_ms(void)
{
    if (!cam.inited || cam.last_jpeg_len == 0) return 1 << 30;
    return (int)(now_ms() - cam.last_jpeg_ms);
}
