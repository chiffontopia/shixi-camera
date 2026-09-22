/*
 * camtest.c — 摄像头诊断工具（跑在开发板上）
 *
 * 作用：不依赖主程序，直接把 V4L2 摄像头的能力、支持的格式/分辨率、
 *       实际采集帧率打印出来，并保存一帧图片，方便定位问题。
 *
 * 用法：
 *   ./camtest              扫描所有 /dev/videoN 并逐个说明
 *   ./camtest /dev/video7  对指定设备做完整采集测试并抓一帧到 /tmp/cam_frame.jpg
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <stdint.h>
#include <linux/videodev2.h>
#include "v4l2compat.h"

#define REQ_W 640
#define REQ_H 480
#define NBUFS 4

static unsigned long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
}

static const char *fourcc_str(uint32_t f, char *buf)
{
    buf[0] = (char)(f & 0xFF);
    buf[1] = (char)((f >> 8) & 0xFF);
    buf[2] = (char)((f >> 16) & 0xFF);
    buf[3] = (char)((f >> 24) & 0xFF);
    buf[4] = 0;
    return buf;
}

/* 扫描并打印所有 video 设备 */
static void scan_all(void)
{
    char path[32];
    int found_cam = 0;
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int fd = open(path, O_RDWR);
        if (fd < 0) fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
            printf("%-14s 打开成功但 QUERYCAP 失败 (%s)\n", path, strerror(errno));
            close(fd);
            continue;
        }
        uint32_t caps = v4l2_device_caps(&cap);
        printf("%-14s driver=%-16s card=%-20s bus=%s\n", path,
               (char *)cap.driver, (char *)cap.card, (char *)cap.bus_info);
        printf("               caps=0x%08x %s%s\n", caps,
               (caps & V4L2_CAP_VIDEO_CAPTURE) ? "CAPTURE " : "",
               (caps & V4L2_CAP_STREAMING) ? "STREAMING " : "");
        if (caps & V4L2_CAP_VIDEO_CAPTURE) {
            found_cam = 1;
            struct v4l2_fmtdesc f;
            for (int k = 0; k < 16; k++) {
                memset(&f, 0, sizeof(f));
                f.index = k;
                f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (ioctl(fd, VIDIOC_ENUM_FMT, &f) != 0) break;
                char fc[8];
                printf("              格式[%d] %s  %s\n", k, fourcc_str(f.pixelformat, fc), f.description);
                struct v4l2_frmsizeenum fs;
                for (int m = 0; m < 12; m++) {
                    memset(&fs, 0, sizeof(fs));
                    fs.index = m;
                    fs.pixel_format = f.pixelformat;
                    if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) != 0) break;
                    if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                        printf("                  %ux%u\n", fs.discrete.width, fs.discrete.height);
                    else
                        printf("                  范围 %ux%u - %ux%u\n", fs.stepwise.min_width,
                               fs.stepwise.min_height, fs.stepwise.max_width, fs.stepwise.max_height);
                }
            }
        }
        close(fd);
    }
    if (!found_cam) {
        printf("\n没有找到支持 Video Capture 的设备。\n");
        printf("请检查：1) 摄像头是否插在 USB Host 口  2) 板上 5V 供电是否足够（建议用电源适配器）\n");
        printf("        3) 内核日志 dmesg 是否有枚举失败（error -32 / unable to enumerate）\n");
    }
}

/* 对单个设备做完整采集测试 */
static int test_device(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) { printf("打开 %s 失败: %s\n", path, strerror(errno)); return -1; }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) { printf("QUERYCAP 失败\n"); close(fd); return -1; }
    char fc[8];
    printf("设备: %s  driver=%s\n", path, (char *)cap.driver);

    uint32_t fmt = 0;
    const char *fmt_name = "";
    struct v4l2_format f;
    uint32_t try_list[2] = { V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV };
    for (int t = 0; t < 2; t++) {
        memset(&f, 0, sizeof(f));
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        f.fmt.pix.width = REQ_W;
        f.fmt.pix.height = REQ_H;
        f.fmt.pix.pixelformat = try_list[t];
        f.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(fd, VIDIOC_S_FMT, &f) == 0 && f.fmt.pix.pixelformat == try_list[t]) {
            fmt = try_list[t];
            fmt_name = (t == 0) ? "MJPEG" : "YUYV";
            break;
        }
    }
    if (!fmt) { printf("既不支持 MJPEG 也不支持 YUYV\n"); close(fd); return -1; }
    printf("协商格式: %s %ux%u\n", fmt_name, f.fmt.pix.width, f.fmt.pix.height);

    int old_abi = v4l2_detect_old_abi(fd);
    printf("v4l2_buffer ABI: %s\n", old_abi ? "内核 3.4 老布局(68B)" : "新布局");

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = NBUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        printf("REQBUFS 失败: %s (errno=%d)\n", strerror(errno), errno);
        close(fd);
        return -1;
    }
    printf("缓冲区: %u 个\n", req.count);

    void *bufs[NBUFS];
    size_t lens[NBUFS];
    for (unsigned i = 0; i < req.count && i < NBUFS; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (v4l2_ioctl_querybuf(fd, &b, old_abi) != 0) {
            printf("QUERYBUF %u 失败: %s (errno=%d)\n", i, strerror(errno), errno);
            printf("  req.count=%u type=%u memory=%u index=%u\n", req.count, b.type, b.memory, b.index);
            close(fd);
            return -1;
        }
        bufs[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        lens[i] = b.length;
        if (bufs[i] == MAP_FAILED) { printf("mmap %u 失败\n", i); close(fd); return -1; }
        v4l2_ioctl_qbuf(fd, &b, old_abi);
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) { printf("STREAMON 失败: %s\n", strerror(errno)); close(fd); return -1; }
    printf("开始采集…\n");

    int got = 0;
    unsigned long long t0 = now_ms();
    int saved = 0;
    for (int i = 0; i < 60; i++) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 1500) <= 0) { printf("第 %d 帧超时（摄像头没有出帧）\n", i); break; }
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (v4l2_ioctl_dqbuf(fd, &b, old_abi) != 0) { printf("DQBUF 失败: %s\n", strerror(errno)); break; }
        got++;
        if (!saved && b.bytesused > 0) {
            const char *out = "/tmp/cam_frame.jpg";
            FILE *fp = fopen(out, "wb");
            if (fp) {
                fwrite(bufs[b.index], 1, b.bytesused, fp);
                fclose(fp);
                printf("已保存一帧: %s (%u 字节)\n", out, b.bytesused);
                saved = 1;
            }
        }
        v4l2_ioctl_qbuf(fd, &b, old_abi);
    }
    unsigned long long dt = now_ms() - t0;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (unsigned i = 0; i < req.count && i < NBUFS; i++) munmap(bufs[i], lens[i]);
    close(fd);

    printf("\n结果: 收到 %d 帧，用时 %llu ms => %.1f fps\n", got, dt, dt ? got * 1000.0 / dt : 0.0);
    if (got == 0) printf("摄像头没有出帧：可能是被别的程序占用，或供电/带宽问题\n");
    return got > 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc >= 2) return test_device(argv[1]) == 0 ? 0 : 1;
    scan_all();
    printf("\n提示: 用 ./camtest /dev/videoN 对某个设备做采集测试\n");
    return 0;
}
