/*
 * v4l2mock.c — 主机端用 LD_PRELOAD 伪造一个 UVC 摄像头，用来测试真实的 V4L2 采集代码路径
 *
 * 背景：主机上没法加载 v4l2loopback（WSL2 没有内核构建树），所以这里拦截
 *      open/ioctl/mmap/poll/stat 等调用，把 /dev/videoN 伪装成一个支持
 *      MJPEG + YUYV 的摄像头，帧数据从目录里的 JPEG 文件循环取。
 *
 * 用法：
 *   MOCK_VIDEO_DIR=/tmp/mockframes LD_PRELOAD=./bin/v4l2mock ./bin/shixi_host
 *   （可选 MOCK_VIDEO_FPS=15 控制出帧速度）
 *
 * 只用于测试，不参与板子上的编译。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <dirent.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <stdint.h>
#include <linux/videodev2.h>

#define MOCK_MAX_FD 8
#define NBUFS 4
/* 单缓冲区大小：要能放下 640x480 的 YUYV(614400B) 或 MJPEG 帧 */
#define BUFSZ (700 * 1024)
#define MAX_FRAMES 64
#define MAX_FRAME_BYTES (400 * 1024)

typedef struct {
    int   used;
    int   fd;
    int   streaming;
    uint32_t fmt;
    int   w, h;
    int   nbufs;
    int   queued[NBUFS];
    int   nqueued;
    int   next_frame;
    unsigned long long last_frame_ms;
    unsigned char *mem;          /* 模拟的 mmap 区域 */
} MockDev;

static MockDev devs[MOCK_MAX_FD];
static unsigned char *frames[MAX_FRAMES];
static int frame_len[MAX_FRAMES];
static int nframes = 0;
static int fps = 10;
static int force_yuyv = 0;      /* MOCK_VIDEO_FORCE_YUYV=1：模拟只支持 YUYV 的摄像头 */
static unsigned long long absent_until = 0;   /* MOCK_VIDEO_DELAY=秒：模拟"过一会儿才插上" */
static int die_after = 0;                     /* MOCK_VIDEO_DIE_AFTER=N：出 N 帧后不再出帧（模拟掉线） */
static int frames_served = 0;
static int inited = 0;

static int (*real_open)(const char *, int, ...);
static int (*real_ioctl)(int, unsigned long, ...);
static int (*real_close)(int);
static int (*real_stat)(const char *, struct stat *);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int (*real_poll)(struct pollfd *, nfds_t, int);

static unsigned long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
}

static void load_frames(void)
{
    if (inited) return;
    inited = 1;
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    if (!real_close) real_close = dlsym(RTLD_NEXT, "close");
    if (!real_stat) real_stat = dlsym(RTLD_NEXT, "stat");
    if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");
    if (!real_poll) real_poll = dlsym(RTLD_NEXT, "poll");

    const char *dir = getenv("MOCK_VIDEO_DIR");
    if (!dir) dir = "/tmp/mockframes";
    const char *f = getenv("MOCK_VIDEO_FPS");
    if (f) fps = atoi(f);
    if (fps <= 0) fps = 10;
    if (getenv("MOCK_VIDEO_FORCE_YUYV")) force_yuyv = 1;
    const char *dl = getenv("MOCK_VIDEO_DELAY");
    if (dl) absent_until = now_ms() + (unsigned long long)atoi(dl) * 1000ULL;
    const char *da = getenv("MOCK_VIDEO_DIE_AFTER");
    if (da) die_after = atoi(da);

    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[512];
    while ((e = readdir(d)) != NULL && nframes < MAX_FRAMES) {
        if (e->d_name[0] == '.') continue;
        size_t n = strlen(e->d_name);
        if (n < 4 || (strcasecmp(e->d_name + n - 4, ".jpg") && strcasecmp(e->d_name + n - 4, ".jpeg")))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        int fd = real_open(path, O_RDONLY);
        if (fd < 0) continue;
        int len = (int)lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        if (len > 0 && len <= MAX_FRAME_BYTES) {
            frames[nframes] = malloc(len);
            if (read(fd, frames[nframes], len) == len) frame_len[nframes] = len;
            if (frame_len[nframes]) nframes++;
        }
        real_close(fd);
    }
    closedir(d);
    fprintf(stderr, "[v4l2mock] 载入 %d 帧 MJPEG 测试数据 (fps=%d)\n", nframes, fps);
}

/* /dev/videoN 判定：只认这一族路径，避免影响别的文件 */
static int is_video_path(const char *path)
{
    if (!path) return 0;
    if (strncmp(path, "/dev/video", 10) != 0) return 0;
    const char *p = path + 10;
    while (*p >= '0' && *p <= '9') p++;
    if (*p != 0 || p == path + 10) return 0;
    if (absent_until && now_ms() < absent_until) return 0;   /* 模拟尚未插入 */
    return 1;
}

static MockDev *find_dev(int fd)
{
    for (int i = 0; i < MOCK_MAX_FD; i++)
        if (devs[i].used && devs[i].fd == fd) return &devs[i];
    return NULL;
}

static MockDev *alloc_dev(int fd)
{
    for (int i = 0; i < MOCK_MAX_FD; i++) {
        if (!devs[i].used) {
            memset(&devs[i], 0, sizeof(MockDev));
            devs[i].used = 1;
            devs[i].fd = fd;
            devs[i].fmt = V4L2_PIX_FMT_MJPEG;
            devs[i].w = 640;
            devs[i].h = 480;
            devs[i].nbufs = NBUFS;
            devs[i].mem = mmap(NULL, (size_t)BUFSZ * NBUFS, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            return &devs[i];
        }
    }
    return NULL;
}

int open(const char *path, int flags, ...)
{
    if (!real_open) load_frames();
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (is_video_path(path)) {
        /* 用 /dev/null 充当真实 fd */
        int fd = real_open("/dev/null", O_RDWR);
        if (fd < 0) return -1;
        MockDev *d = alloc_dev(fd);
        if (d) fprintf(stderr, "[v4l2mock] 打开虚拟摄像头 %s -> fd %d\n", path, fd);
        return fd;
    }
    return real_open(path, flags, mode);
}

int stat(const char *path, struct stat *buf)
{
    if (!real_stat) load_frames();
    if (is_video_path(path)) {
        memset(buf, 0, sizeof(*buf));
        buf->st_mode = S_IFCHR | 0666;
        buf->st_rdev = 0x81;      /* 假装是字符设备 */
        return 0;
    }
    return real_stat(path, buf);
}

int close(int fd)
{
    if (!real_close) load_frames();
    MockDev *d = find_dev(fd);
    if (d) {
        if (d->mem) munmap(d->mem, (size_t)BUFSZ * NBUFS);
        d->used = 0;
    }
    return real_close(fd);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (!real_mmap) load_frames();
    MockDev *d = find_dev(fd);
    if (d && d->mem) {
        /* 返回这段匿名内存中对应 offset 的位置 */
        size_t o = (size_t)off;
        if (o + len > (size_t)BUFSZ * NBUFS) return MAP_FAILED;
        return d->mem + o;
    }
    return real_mmap(addr, len, prot, flags, fd, off);
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (!real_poll) load_frames();
    /* 只要包含虚拟摄像头，就按帧率报告可读 */
    for (nfds_t i = 0; i < nfds; i++) {
        MockDev *d = find_dev(fds[i].fd);
        if (d) {
            if (!d->streaming || nframes == 0) {
                usleep(timeout > 0 ? timeout * 1000 : 10000);
                return 0;
            }
            if (die_after > 0 && frames_served >= die_after) {
                /* 模拟掉线：poll 报错，DQBUF 也返回 ENODEV */
                fds[i].revents = POLLERR;
                return 1;
            }
            unsigned long long now = now_ms();
            unsigned long long interval = (unsigned long long)(1000 / (fps > 0 ? fps : 10));
            if (now - d->last_frame_ms >= interval) {
                d->last_frame_ms = now;
                fds[i].revents = POLLIN;
                return 1;
            }
            unsigned long long wait = interval - (now - d->last_frame_ms);
            if (timeout >= 0 && wait > (unsigned long long)timeout) wait = (unsigned long long)timeout;
            usleep((useconds_t)(wait * 1000));
            return 0;
        }
    }
    return real_poll(fds, nfds, timeout);
}

static int mock_ioctl(MockDev *d, unsigned long req, void *arg)
{
    switch (req) {
    case VIDIOC_QUERYCAP: {
        struct v4l2_capability *c = (struct v4l2_capability *)arg;
        memset(c, 0, sizeof(*c));
        strcpy((char *)c->driver, "uvcvideo-mock");
        strcpy((char *)c->card, "Mock USB Camera");
        strcpy((char *)c->bus_info, "usb-mock-1");
        c->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_DEVICE_CAPS;
        c->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        return 0;
    }
    case VIDIOC_ENUM_FMT: {
        struct v4l2_fmtdesc *f = (struct v4l2_fmtdesc *)arg;
        if (f->index == 0) {
            f->pixelformat = V4L2_PIX_FMT_MJPEG;
            strcpy((char *)f->description, "Motion-JPEG");
            return 0;
        }
        if (f->index == 1) {
            f->pixelformat = V4L2_PIX_FMT_YUYV;
            strcpy((char *)f->description, "YUYV 4:2:2");
            return 0;
        }
        errno = EINVAL;
        return -1;
    }
    case VIDIOC_ENUM_FRAMESIZES: {
        struct v4l2_frmsizeenum *fs = (struct v4l2_frmsizeenum *)arg;
        if (fs->index > 0) { errno = EINVAL; return -1; }
        fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
        fs->discrete.width = 640;
        fs->discrete.height = 480;
        return 0;
    }
    case VIDIOC_S_FMT:
    case VIDIOC_G_FMT: {
        struct v4l2_format *f = (struct v4l2_format *)arg;
        if (req == VIDIOC_S_FMT) {
            /* 模拟驱动：接受 MJPEG/YUYV，尺寸固定 640x480 */
            if (force_yuyv) {
                /* 模拟"只支持 YUYV"的摄像头：无论请求什么都回 YUYV */
                f->fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
            } else if (f->fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG &&
                       f->fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
                f->fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
            }
            d->fmt = f->fmt.pix.pixelformat;
            if (f->fmt.pix.width != 640 || f->fmt.pix.height != 480) {
                f->fmt.pix.width = 640;
                f->fmt.pix.height = 480;
            }
        } else {
            f->fmt.pix.pixelformat = d->fmt;
            f->fmt.pix.width = 640;
            f->fmt.pix.height = 480;
        }
        d->w = (int)f->fmt.pix.width;
        d->h = (int)f->fmt.pix.height;
        f->fmt.pix.field = V4L2_FIELD_NONE;
        f->fmt.pix.bytesperline = (d->fmt == V4L2_PIX_FMT_YUYV) ? d->w * 2 : 0;
        f->fmt.pix.sizeimage = (d->fmt == V4L2_PIX_FMT_YUYV) ? (uint32_t)(d->w * d->h * 2) : MAX_FRAME_BYTES;
        return 0;
    }
    case VIDIOC_REQBUFS: {
        struct v4l2_requestbuffers *r = (struct v4l2_requestbuffers *)arg;
        if (r->count < 2) { errno = EINVAL; return -1; }
        r->count = NBUFS;
        d->nbufs = NBUFS;
        d->nqueued = 0;
        return 0;
    }
    case VIDIOC_QUERYBUF: {
        struct v4l2_buffer *b = (struct v4l2_buffer *)arg;
        if (b->index >= (unsigned)d->nbufs) { errno = EINVAL; return -1; }
        b->length = BUFSZ;
        b->m.offset = b->index * BUFSZ;
        b->memory = V4L2_MEMORY_MMAP;
        return 0;
    }
    case VIDIOC_QBUF: {
        struct v4l2_buffer *b = (struct v4l2_buffer *)arg;
        if (d->nqueued < NBUFS) d->queued[d->nqueued++] = (int)b->index;
        return 0;
    }
    case VIDIOC_DQBUF: {
        struct v4l2_buffer *b = (struct v4l2_buffer *)arg;
        if (die_after > 0 && frames_served >= die_after) { errno = ENODEV; return -1; }
        if (d->nqueued == 0) { errno = EAGAIN; return -1; }
        int idx = d->queued[0];
        memmove(&d->queued[0], &d->queued[1], sizeof(int) * (size_t)(d->nqueued - 1));
        d->nqueued--;
        memset(b, 0, sizeof(*b));
        b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b->memory = V4L2_MEMORY_MMAP;
        b->index = (unsigned)idx;

        if (nframes > 0) {
            int k = d->next_frame % nframes;
            d->next_frame++;
            unsigned char *buf = d->mem + (size_t)idx * BUFSZ;
            if (d->fmt == V4L2_PIX_FMT_MJPEG) {
                frames_served++;
                int len = frame_len[k];
                if (len > BUFSZ) len = BUFSZ;
                memcpy(buf, frames[k], (size_t)len);
                b->bytesused = (uint32_t)len;
            } else {
                frames_served++;
                /* YUYV：把 JPEG 解出来的第一帧像素不够用，这里生成渐变条纹 */
                uint32_t len = (uint32_t)(d->w * d->h * 2);
                for (uint32_t y = 0; y < (uint32_t)d->h; y++) {
                    for (uint32_t x = 0; x < (uint32_t)d->w; x += 2) {
                        unsigned char *p = buf + (size_t)y * d->w * 2 + x * 2;
                        p[0] = (unsigned char)((x + d->next_frame * 8) & 0xFF);
                        p[1] = (unsigned char)(128 + ((y / 8) & 1) * 40);
                        p[2] = (unsigned char)((x + 40 + d->next_frame * 8) & 0xFF);
                        p[3] = (unsigned char)(128 - ((y / 8) & 1) * 40);
                    }
                }
                b->bytesused = len;
            }
        } else {
            b->bytesused = 0;
        }
        return 0;
    }
    case VIDIOC_STREAMON:
        d->streaming = 1;
        d->last_frame_ms = 0;
        return 0;
    case VIDIOC_STREAMOFF:
        d->streaming = 0;
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

int ioctl(int fd, unsigned long req, ...)
{
    if (!real_ioctl) load_frames();
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    MockDev *d = find_dev(fd);
    if (d) return mock_ioctl(d, req, arg);
    return real_ioctl(fd, req, arg);
}
