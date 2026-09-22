/*
 * image.c — 图片读写与缩放实现
 */
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#include "../third_party/stb_image.h"
#include "../third_party/stb_image_write.h"

unsigned char *file_read_all(const char *path, int *size_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return NULL; }
    int size = (int)st.st_size;
    unsigned char *buf = (unsigned char *)malloc(size);
    if (!buf) { close(fd); return NULL; }
    int got = 0;
    while (got < size) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n <= 0) break;
        got += (int)n;
    }
    close(fd);
    if (got != size) { free(buf); return NULL; }
    if (size_out) *size_out = size;
    return buf;
}

int file_write_all(const char *path, const void *data, int size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    const unsigned char *p = (const unsigned char *)data;
    int done = 0;
    while (done < size) {
        ssize_t n = write(fd, p + done, size - done);
        if (n <= 0) { close(fd); return -1; }
        done += (int)n;
    }
    close(fd);
    return 0;
}

/* RGB(3字节/像素) -> Surface(0x00RRGGBB) */
static Surface *rgb_to_surface(const unsigned char *rgb, int w, int h)
{
    Surface *s = gfx_surface_new(w, h);
    if (!s) return NULL;
    for (int y = 0; y < h; y++) {
        const unsigned char *src = rgb + (size_t)y * w * 3;
        uint32_t *dst = s->px + (size_t)y * s->stride;
        for (int x = 0; x < w; x++)
            dst[x] = ((uint32_t)src[x * 3] << 16) | ((uint32_t)src[x * 3 + 1] << 8) | src[x * 3 + 2];
    }
    return s;
}

Surface *image_load_mem(const unsigned char *data, int len)
{
    int w = 0, h = 0, comp = 0;
    unsigned char *rgb = stbi_load_from_memory(data, len, &w, &h, &comp, 3);
    if (!rgb) return NULL;
    Surface *s = rgb_to_surface(rgb, w, h);
    stbi_image_free(rgb);
    return s;
}

Surface *image_load_file(const char *path)
{
    int len = 0;
    unsigned char *buf = file_read_all(path, &len);
    if (!buf) return NULL;
    Surface *s = image_load_mem(buf, len);
    free(buf);
    return s;
}

/* 双线性缩放 */
Surface *image_scale(const Surface *src, int w, int h)
{
    if (!src || w <= 0 || h <= 0) return NULL;
    Surface *dst = gfx_surface_new(w, h);
    if (!dst) return NULL;
    /* 半像素中心对齐，避免整体偏移 */
    double sx = (double)src->w / w, sy = (double)src->h / h;
    for (int y = 0; y < h; y++) {
        double fy = (y + 0.5) * sy - 0.5;
        int y0 = (int)fy;
        double wy = fy - y0;
        if (y0 < 0) { y0 = 0; wy = 0; }
        int y1 = y0 + 1; if (y1 >= src->h) { y1 = src->h - 1; }
        const uint32_t *r0 = src->px + (size_t)y0 * src->stride;
        const uint32_t *r1 = src->px + (size_t)y1 * src->stride;
        uint32_t *drow = dst->px + (size_t)y * dst->stride;
        for (int x = 0; x < w; x++) {
            double fx = (x + 0.5) * sx - 0.5;
            int x0 = (int)fx;
            double wx = fx - x0;
            if (x0 < 0) { x0 = 0; wx = 0; }
            int x1 = x0 + 1; if (x1 >= src->w) { x1 = src->w - 1; }
            uint32_t c00 = r0[x0], c01 = r0[x1], c10 = r1[x0], c11 = r1[x1];
            int w00 = (int)((1 - wx) * (1 - wy) * 256.0);
            int w01 = (int)(wx * (1 - wy) * 256.0);
            int w10 = (int)((1 - wx) * wy * 256.0);
            int w11 = 256 - w00 - w01 - w10;
            int r = (((c00 >> 16) & 0xFF) * w00 + ((c01 >> 16) & 0xFF) * w01 +
                     ((c10 >> 16) & 0xFF) * w10 + ((c11 >> 16) & 0xFF) * w11) >> 8;
            int g = (((c00 >> 8) & 0xFF) * w00 + ((c01 >> 8) & 0xFF) * w01 +
                     ((c10 >> 8) & 0xFF) * w10 + ((c11 >> 8) & 0xFF) * w11) >> 8;
            int b = ((c00 & 0xFF) * w00 + (c01 & 0xFF) * w01 +
                     (c10 & 0xFF) * w10 + (c11 & 0xFF) * w11) >> 8;
            drow[x] = ((uint32_t)(r & 0xFF) << 16) | ((uint32_t)(g & 0xFF) << 8) | (uint32_t)(b & 0xFF);
        }
    }
    return dst;
}

Surface *image_thumbnail(const Surface *src, int w, int h)
{
    if (!src || w <= 0 || h <= 0) return NULL;
    /* 等比 cover：先按短边缩放，再居中裁剪 */
    double sar = (double)src->w / src->h, dar = (double)w / h;
    int tw, th;
    if (sar > dar) { th = h; tw = (int)(h * sar + 0.5); }
    else           { tw = w; th = (int)(w / sar + 0.5); }
    Surface *tmp = image_scale(src, tw, th);
    if (!tmp) return NULL;
    Surface *out = gfx_surface_new(w, h);
    if (!out) { gfx_surface_free(tmp); return NULL; }
    int ox = (tw - w) / 2, oy = (th - h) / 2;
    for (int y = 0; y < h; y++) {
        const uint32_t *srow = tmp->px + (size_t)(y + oy) * tmp->stride + ox;
        uint32_t *drow = out->px + (size_t)y * out->stride;
        memcpy(drow, srow, (size_t)w * 4);
    }
    gfx_surface_free(tmp);
    return out;
}

/* ---- JPEG 编码：写进内存缓冲区，再落盘 ---- */
typedef struct {
    unsigned char *buf;
    int len, cap;
    int failed;
} MemSink;

static void mem_write_cb(void *ctx, void *data, int size)
{
    MemSink *m = (MemSink *)ctx;
    if (m->failed) return;
    if (m->len + size > m->cap) {
        int cap = m->cap ? m->cap * 2 : 65536;
        while (cap < m->len + size) cap *= 2;
        unsigned char *nb = (unsigned char *)realloc(m->buf, cap);
        if (!nb) { m->failed = 1; return; }
        m->buf = nb; m->cap = cap;
    }
    memcpy(m->buf + m->len, data, size);
    m->len += size;
}

int image_save_jpeg(const char *path, const Surface *s, int quality)
{
    unsigned char *buf = NULL;
    int len = 0;
    if (image_encode_jpeg_mem(s, quality, &buf, &len) != 0) return -1;
    int ret = file_write_all(path, buf, len);
    free(buf);
    return ret;
}

int image_encode_jpeg_mem(const Surface *s, int quality, unsigned char **out, int *out_len)
{
    if (!s || !out || !out_len) return -1;
    unsigned char *rgb = (unsigned char *)malloc((size_t)s->w * s->h * 3);
    if (!rgb) return -1;
    for (int y = 0; y < s->h; y++) {
        const uint32_t *srow = s->px + (size_t)y * s->stride;
        unsigned char *o = rgb + (size_t)y * s->w * 3;
        for (int x = 0; x < s->w; x++) {
            uint32_t c = srow[x];
            o[x * 3 + 0] = (c >> 16) & 0xFF;
            o[x * 3 + 1] = (c >> 8) & 0xFF;
            o[x * 3 + 2] = c & 0xFF;
        }
    }
    MemSink sink = { NULL, 0, 0, 0 };
    int ok = stbi_write_jpg_to_func(mem_write_cb, &sink, s->w, s->h, 3, rgb, quality);
    free(rgb);
    if (!ok || sink.failed || !sink.buf) { free(sink.buf); return -1; }
    *out = sink.buf;
    *out_len = sink.len;
    return 0;
}
