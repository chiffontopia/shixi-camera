/*
 * v4l2compat.h — 兼容老内核的 V4L2 缓冲区 ABI
 *
 * 问题：VIDIOC_QUERYBUF / QBUF / DQBUF 的 ioctl 号用 _IOWR 把
 *      sizeof(struct v4l2_buffer) 编进了命令码里。板子内核是 3.4，
 *      其中 struct v4l2_buffer 是 68 字节（时间戳是 32 位 timeval，8 字节）；
 *      而新版内核头文件里时间戳变成 16 字节，结构体变成 80 字节，
 *      于是命令码从 0xc0445609 变成 0xc0505609，老内核认不出来，直接返回
 *      ENOTTY（errno 25）——表现就是"能协商格式、一取缓冲区就失败"。
 *
 * 做法：显式定义 3.4 的旧布局，并在打开设备时探测一次用哪种 ABI，
 *      这样在新老内核上都能跑（主机模拟器走新 ABI）。
 */
#ifndef SHIXI_V4L2COMPAT_H
#define SHIXI_V4L2COMPAT_H

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

/* ---- 内核 3.4 的 struct v4l2_buffer 布局（32 位，共 68 字节） ---- */
struct v4l2_buffer_old {
    uint32_t index;
    uint32_t type;
    uint32_t bytesused;
    uint32_t flags;
    uint32_t field;
    int32_t  timestamp_sec;
    int32_t  timestamp_usec;
    uint8_t  timecode[16];
    uint32_t sequence;
    uint32_t memory;
    uint32_t m_offset;
    uint32_t length;
    uint32_t input;
    uint32_t reserved;
};

/* 编译期校验：必须正好 68 字节，否则说明结构体定义写错了 */
typedef char v4l2_buffer_old_size_check[(sizeof(struct v4l2_buffer_old) == 68) ? 1 : -1];

#define VIDIOC_QUERYBUF_OLD _IOWR('V',  9, struct v4l2_buffer_old)
#define VIDIOC_QBUF_OLD     _IOWR('V', 15, struct v4l2_buffer_old)
#define VIDIOC_DQBUF_OLD    _IOWR('V', 17, struct v4l2_buffer_old)

static inline void v4l2_buf_to_old(const struct v4l2_buffer *n, struct v4l2_buffer_old *o)
{
    memset(o, 0, sizeof(*o));
    o->index     = n->index;
    o->type      = (uint32_t)n->type;
    o->bytesused = n->bytesused;
    o->flags     = n->flags;
    o->field     = (uint32_t)n->field;
    o->sequence  = n->sequence;
    o->memory    = (uint32_t)n->memory;
    o->m_offset  = n->m.offset;
    o->length    = n->length;
}

static inline void v4l2_buf_from_old(struct v4l2_buffer *n, const struct v4l2_buffer_old *o)
{
    n->index     = o->index;
    n->type      = o->type;
    n->bytesused = o->bytesused;
    n->flags     = o->flags;
    n->field     = o->field;
    n->sequence  = o->sequence;
    n->memory    = o->memory;
    n->m.offset  = o->m_offset;
    n->length    = o->length;
}

/*
 * 探测设备需要哪种 ABI：此时还没有申请缓冲区，
 * 用对的结构体会得到 EINVAL，用错的结构体得到 ENOTTY。
 */
static inline int v4l2_detect_old_abi(int fd)
{
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = 0;
    errno = 0;
    if (ioctl(fd, VIDIOC_QUERYBUF, &b) == 0) return 0;   /* 居然成功（已分配过缓冲）：按新 ABI */
    return errno == ENOTTY ? 1 : 0;
}

static inline int v4l2_ioctl_querybuf(int fd, struct v4l2_buffer *b, int old_abi)
{
    if (!old_abi) return ioctl(fd, VIDIOC_QUERYBUF, b);
    struct v4l2_buffer_old o;
    v4l2_buf_to_old(b, &o);
    int r = ioctl(fd, VIDIOC_QUERYBUF_OLD, &o);
    if (r == 0) v4l2_buf_from_old(b, &o);
    return r;
}

static inline int v4l2_ioctl_qbuf(int fd, struct v4l2_buffer *b, int old_abi)
{
    if (!old_abi) return ioctl(fd, VIDIOC_QBUF, b);
    struct v4l2_buffer_old o;
    v4l2_buf_to_old(b, &o);
    int r = ioctl(fd, VIDIOC_QBUF_OLD, &o);
    if (r == 0) v4l2_buf_from_old(b, &o);
    return r;
}

static inline int v4l2_ioctl_dqbuf(int fd, struct v4l2_buffer *b, int old_abi)
{
    if (!old_abi) return ioctl(fd, VIDIOC_DQBUF, b);
    struct v4l2_buffer_old o;
    v4l2_buf_to_old(b, &o);
    int r = ioctl(fd, VIDIOC_DQBUF_OLD, &o);
    if (r == 0) v4l2_buf_from_old(b, &o);
    return r;
}

/*
 * 取设备能力：内核 3.4 的 videodev2.h 还没有 device_caps，
 * 那时 capabilities 直接就是设备能力；新内核才把驱动能力与设备能力分开。
 * 用宏判断，老工具链（arm-linux-gcc 5.4.0）与新工具链都能编。
 */
static inline uint32_t v4l2_device_caps(const struct v4l2_capability *cap)
{
#ifdef V4L2_CAP_DEVICE_CAPS
    if (cap->capabilities & V4L2_CAP_DEVICE_CAPS) return cap->device_caps;
#endif
    return cap->capabilities;
}

#endif /* SHIXI_V4L2COMPAT_H */
