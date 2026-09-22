/*
 * avi.c — MJPEG AVI 读写实现
 *
 * AVI 结构（只用到最小必要集合）：
 *   RIFF 'AVI '
 *     LIST 'hdrl'
 *       'avih'  MainAVIHeader(56B)
 *       LIST 'strl'
 *         'strh' AVIStreamHeader(56B)  fccType='vids' fccHandler='MJPG'
 *         'strf' BITMAPINFOHEADER(40B) biCompression='MJPG'
 *     LIST 'movi'
 *       '00dc' <jpeg> ...           每帧一个 chunk
 *     'idx1'  索引（seek 用）
 */
#include "avi.h"
#include "image.h"        /* file_read_all / file_write_all */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* ---------------- 小端读写 ---------------- */
static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 读 16 位小端（保留给需要解析 wPriority 等字段的场合） */
static uint16_t get_u16(const unsigned char *p) __attribute__((unused));
static uint16_t get_u16(const unsigned char *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* ================================================================== */
/* 写                                                                  */
/* ================================================================== */
struct AviWriter {
    int   fd;
    char  path[300];
    int   width, height, fps;
    long  riff_size_pos;      /* 'RIFF' size 字段位置 */
    long  movi_size_pos;      /* 'movi' LIST size 字段位置 */
    long  movi_data_pos;      /* 第一个 chunk 的位置 */
    long  avih_pos, strh_pos, strf_pos;
    int   frames;
    long  bytes;
    int   failed;
    /* 索引（帧数较多时用动态数组） */
    long *idx_off;
    int  *idx_len;
    int   idx_cap;
    long  pos;                /* 当前写入位置 */
};

static int w_write(AviWriter *w, const void *data, int len)
{
    if (w->failed) return -1;
    const unsigned char *p = (const unsigned char *)data;
    int done = 0;
    while (done < len) {
        ssize_t n = write(w->fd, p + done, len - done);
        if (n <= 0) { w->failed = 1; return -1; }
        done += (int)n;
    }
    w->pos += len;
    return 0;
}

static int w_patch(AviWriter *w, long at, const void *data, int len)
{
    if (pwrite(w->fd, data, len, at) != len) { w->failed = 1; return -1; }
    return 0;
}

AviWriter *avi_writer_open(const char *path, int width, int height, int fps)
{
    if (width <= 0 || height <= 0 || fps <= 0) return NULL;
    AviWriter *w = (AviWriter *)calloc(1, sizeof(AviWriter));
    if (!w) return NULL;
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->width = width; w->height = height; w->fps = fps;
    w->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (w->fd < 0) { free(w); return NULL; }

    unsigned char hdr[256];
    unsigned char *p = hdr;
    /* RIFF header */
    memcpy(p, "RIFF", 4); p += 4;
    w->riff_size_pos = w->pos + (p - hdr);
    put_u32(p, 0); p += 4;
    memcpy(p, "AVI ", 4); p += 4;
    /* LIST hdrl */
    memcpy(p, "LIST", 4); p += 4;
    put_u32(p, 4 + 8 + 56 + 8 + 8 + 56 + 8 + 40); p += 4;   /* hdrl 数据长度 */
    memcpy(p, "hdrl", 4); p += 4;
    /* avih */
    memcpy(p, "avih", 4); p += 4;
    put_u32(p, 56); p += 4;
    w->avih_pos = w->pos + (p - hdr);
    {
        unsigned char avih[56];
        memset(avih, 0, sizeof(avih));
        put_u32(avih + 0, (uint32_t)(1000000 / fps));      /* dwMicroSecPerFrame */
        put_u32(avih + 4, 0);                              /* dwMaxBytesPerSec (回填) */
        put_u32(avih + 8, 0);                              /* dwPaddingGranularity */
        put_u32(avih + 12, 0x10);                          /* dwFlags: AVIF_HASINDEX */
        put_u32(avih + 16, 0);                             /* dwTotalFrames (回填) */
        put_u32(avih + 20, 0);                             /* dwInitialFrames */
        put_u32(avih + 24, 1);                             /* dwStreams */
        put_u32(avih + 28, 0);                             /* dwSuggestedBufferSize */
        put_u32(avih + 32, (uint32_t)width);
        put_u32(avih + 36, (uint32_t)height);
        memcpy(p, avih, 56); p += 56;
    }
    /* LIST strl */
    memcpy(p, "LIST", 4); p += 4;
    put_u32(p, 4 + 8 + 56 + 8 + 40); p += 4;
    memcpy(p, "strl", 4); p += 4;
    /* strh */
    memcpy(p, "strh", 4); p += 4;
    put_u32(p, 56); p += 4;
    w->strh_pos = w->pos + (p - hdr);
    {
        unsigned char strh[56];
        memset(strh, 0, sizeof(strh));
        memcpy(strh + 0, "vids", 4);
        memcpy(strh + 4, "MJPG", 4);
        put_u32(strh + 8, 0);                     /* dwFlags */
        put_u16(strh + 12, 0);                    /* wPriority */
        put_u16(strh + 14, 0);                    /* wLanguage */
        put_u32(strh + 16, 0);                    /* dwInitialFrames */
        put_u32(strh + 20, 1);                    /* dwScale：每帧 dwScale/dwRate 秒 */
        put_u32(strh + 24, (uint32_t)fps);        /* dwRate = 帧率 */
        put_u32(strh + 28, 0);                    /* dwStart */
        put_u32(strh + 32, 0);                    /* dwLength (回填) */
        put_u32(strh + 36, 0);                    /* dwSuggestedBufferSize */
        put_u32(strh + 40, 0xFFFFFFFF);           /* dwQuality */
        put_u32(strh + 44, 0);                    /* dwSampleSize */
        put_u16(strh + 48, 0); put_u16(strh + 50, 0);
        put_u16(strh + 52, (uint16_t)width);      /* rcFrame */
        put_u16(strh + 54, (uint16_t)height);
        memcpy(p, strh, 56); p += 56;
    }
    /* strf: BITMAPINFOHEADER */
    memcpy(p, "strf", 4); p += 4;
    put_u32(p, 40); p += 4;
    w->strf_pos = w->pos + (p - hdr);
    {
        unsigned char strf[40];
        memset(strf, 0, sizeof(strf));
        put_u32(strf + 0, 40);
        put_u32(strf + 4, (uint32_t)width);
        put_u32(strf + 8, (uint32_t)height);
        put_u16(strf + 12, 1);
        put_u16(strf + 14, 24);
        memcpy(strf + 16, "MJPG", 4);
        put_u32(strf + 20, (uint32_t)(width * height * 3));
        memcpy(p, strf, 40); p += 40;
    }
    /* LIST movi */
    memcpy(p, "LIST", 4); p += 4;
    w->movi_size_pos = w->pos + (p - hdr);
    put_u32(p, 0); p += 4;
    memcpy(p, "movi", 4); p += 4;
    w->movi_data_pos = w->pos + (p - hdr);

    if (w_write(w, hdr, (int)(p - hdr)) != 0) {
        close(w->fd); unlink(path); free(w); return NULL;
    }
    w->idx_cap = 256;
    w->idx_off = (long *)malloc(sizeof(long) * w->idx_cap);
    w->idx_len = (int *)malloc(sizeof(int) * w->idx_cap);
    if (!w->idx_off || !w->idx_len) {
        close(w->fd); unlink(path); free(w->idx_off); free(w->idx_len); free(w);
        return NULL;
    }
    return w;
}

int avi_writer_add(AviWriter *w, const void *jpeg, int len)
{
    if (!w || !jpeg || len <= 0 || w->failed) return -1;
    if (w->frames >= w->idx_cap) {
        int cap = w->idx_cap * 2;
        long *no = (long *)realloc(w->idx_off, sizeof(long) * cap);
        int *nl = (int *)realloc(w->idx_len, sizeof(int) * cap);
        if (!no || !nl) { free(no); free(nl); return -1; }
        w->idx_off = no; w->idx_len = nl; w->idx_cap = cap;
    }
    unsigned char head[8];
    memcpy(head, "00dc", 4);
    put_u32(head + 4, (uint32_t)len);
    long chunk_pos = w->pos;
    if (w_write(w, head, 8) != 0) return -1;
    if (w_write(w, jpeg, len) != 0) return -1;
    if (len & 1) { unsigned char z = 0; if (w_write(w, &z, 1) != 0) return -1; }
    w->idx_off[w->frames] = chunk_pos - w->movi_data_pos;
    w->idx_len[w->frames] = len;
    w->frames++;
    w->bytes += len;
    return 0;
}

void avi_writer_set_fps(AviWriter *w, int fps)
{
    if (!w || fps < 1) return;
    if (fps > 60) fps = 60;
    w->fps = fps;
}

int avi_writer_frames(AviWriter *w) { return w ? w->frames : 0; }
int avi_writer_bytes(AviWriter *w) { return w ? (int)w->bytes : 0; }

int avi_writer_close(AviWriter *w, int abort)
{
    if (!w) return -1;
    int ret = 0;
    if (!abort && !w->failed && w->frames > 0) {
        /* idx1 */
        int idx_len = 16 * w->frames;
        long idx_pos = w->pos;
        unsigned char *idx = (unsigned char *)malloc(idx_len);
        if (!idx) ret = -1;
        else {
            for (int i = 0; i < w->frames; i++) {
                unsigned char *e = idx + i * 16;
                memcpy(e, "00dc", 4);
                put_u32(e + 4, 0x10);                       /* AVIIF_KEYFRAME */
                put_u32(e + 8, (uint32_t)w->idx_off[i]);    /* 相对 movi 数据区 */
                put_u32(e + 12, (uint32_t)w->idx_len[i]);
            }
            unsigned char hdr[8];
            memcpy(hdr, "idx1", 4);
            put_u32(hdr + 4, (uint32_t)idx_len);
            if (w_write(w, hdr, 8) != 0 || w_write(w, idx, idx_len) != 0) ret = -1;
            free(idx);
        }
        /* 回填 avih */
        unsigned char avih[56];
        memset(avih, 0, sizeof(avih));
        long total = w->pos;
        put_u32(avih + 0, (uint32_t)(1000000 / w->fps));
        put_u32(avih + 4, (uint32_t)(w->bytes / (w->frames ? w->frames : 1) * w->fps));
        put_u32(avih + 12, 0x10);
        put_u32(avih + 16, (uint32_t)w->frames);
        put_u32(avih + 24, 1);
        put_u32(avih + 28, (uint32_t)(w->bytes / (w->frames ? w->frames : 1)));
        put_u32(avih + 32, (uint32_t)w->width);
        put_u32(avih + 36, (uint32_t)w->height);
        if (w_patch(w, w->avih_pos, avih, 56) != 0) ret = -1;
        /* 回填 strh */
        unsigned char strh[56];
        memset(strh, 0, sizeof(strh));
        memcpy(strh + 0, "vids", 4);
        memcpy(strh + 4, "MJPG", 4);
        put_u32(strh + 20, 1);
        put_u32(strh + 24, (uint32_t)w->fps);
        put_u32(strh + 32, (uint32_t)w->frames);
        put_u32(strh + 36, (uint32_t)(w->bytes / (w->frames ? w->frames : 1)));
        put_u32(strh + 40, 0xFFFFFFFF);
        put_u16(strh + 52, (uint16_t)w->width);
        put_u16(strh + 54, (uint16_t)w->height);
        if (w_patch(w, w->strh_pos, strh, 56) != 0) ret = -1;
        /* 回填 strf */
        unsigned char strf[40];
        memset(strf, 0, sizeof(strf));
        put_u32(strf + 0, 40);
        put_u32(strf + 4, (uint32_t)w->width);
        put_u32(strf + 8, (uint32_t)w->height);
        put_u16(strf + 12, 1);
        put_u16(strf + 14, 24);
        memcpy(strf + 16, "MJPG", 4);
        put_u32(strf + 20, (uint32_t)(w->width * w->height * 3));
        if (w_patch(w, w->strf_pos, strf, 40) != 0) ret = -1;
        /* 回填 movi 长度：从 movi 数据区到 idx1 之前 */
        unsigned char sz[4];
        put_u32(sz, (uint32_t)(idx_pos - (w->movi_data_pos - 4)));
        if (w_patch(w, w->movi_size_pos, sz, 4) != 0) ret = -1;
        /* 回填 RIFF 长度：文件总长 - 8 */
        put_u32(sz, (uint32_t)(total - 8));
        if (w_patch(w, w->riff_size_pos, sz, 4) != 0) ret = -1;
    }
    close(w->fd);
    if (abort || w->failed || w->frames == 0) {
        unlink(w->path);
        if (!abort) ret = -1;
    }
    free(w->idx_off);
    free(w->idx_len);
    free(w);
    return ret;
}

/* ================================================================== */
/* 读                                                                  */
/* ================================================================== */
struct AviReader {
    unsigned char *data;
    int   size;
    int   width, height, fps, frames;
    long *off;
    int  *len;
    int   cap;
};

static void r_scan(AviReader *r)
{
    /* 找 'movi' 列表，然后顺序扫描 '00dc'/'00db' chunk */
    long movi = -1;
    for (long i = 0; i + 12 < r->size; i++) {
        if (!memcmp(r->data + i, "LIST", 4) && !memcmp(r->data + i + 8, "movi", 4)) {
            movi = i;
            break;
        }
    }
    if (movi < 0) return;
    long p = movi + 12;
    long end = r->size;
    /* 如果 movi LIST 长度可信，用长度限制扫描范围 */
    uint32_t list_len = get_u32(r->data + movi + 4);
    if (list_len > 8 && movi + 8 + (long)list_len <= r->size)
        end = movi + 8 + (long)list_len;
    while (p + 8 <= end) {
        uint32_t clen = get_u32(r->data + p + 4);
        if (!memcmp(r->data + p, "00dc", 4) || !memcmp(r->data + p, "00db", 4)) {
            if (p + 8 + (long)clen > r->size) break;
            if (r->frames >= r->cap) {
                int cap = r->cap ? r->cap * 2 : 128;
                long *no = (long *)realloc(r->off, sizeof(long) * cap);
                int *nl = (int *)realloc(r->len, sizeof(int) * cap);
                if (!no || !nl) { free(no); free(nl); return; }
                r->off = no; r->len = nl; r->cap = cap;
            }
            r->off[r->frames] = p + 8;
            r->len[r->frames] = (int)clen;
            r->frames++;
        }
        p += 8 + clen + (clen & 1);
        if (clen == 0) break;
    }
}

AviReader *avi_open(const char *path)
{
    int size = 0;
    unsigned char *data = file_read_all(path, &size);
    if (!data || size < 64) { free(data); return NULL; }
    AviReader *r = (AviReader *)calloc(1, sizeof(AviReader));
    if (!r) { free(data); return NULL; }
    r->data = data;
    r->size = size;
    r->width = r->height = 0;
    r->fps = 15;
    /* 从 strf 里取宽高 */
    for (int i = 0; i + 40 < size; i++) {
        if (!memcmp(data + i, "strf", 4)) {
            r->width = (int)get_u32(data + i + 12);
            r->height = (int)get_u32(data + i + 16);
            break;
        }
    }
    /* 从 strh 取帧率 (dwScale/dwRate) */
    for (int i = 0; i + 56 < size; i++) {
        if (!memcmp(data + i, "strh", 4) && !memcmp(data + i + 8, "vids", 4)) {
            uint32_t scale = get_u32(data + i + 8 + 20);
            uint32_t rate = get_u32(data + i + 8 + 24);
            if (scale > 0) r->fps = (int)(rate / scale);
            if (r->fps <= 0 || r->fps > 60) r->fps = 15;
            break;
        }
    }
    r_scan(r);
    if (r->frames == 0) { avi_close(r); return NULL; }
    return r;
}

void avi_close(AviReader *r)
{
    if (!r) return;
    free(r->data);
    free(r->off);
    free(r->len);
    free(r);
}

int avi_width(AviReader *r) { return r ? r->width : 0; }
int avi_height(AviReader *r) { return r ? r->height : 0; }
int avi_fps(AviReader *r) { return r ? r->fps : 0; }
int avi_frame_count(AviReader *r) { return r ? r->frames : 0; }

int avi_duration_s(AviReader *r)
{
    if (!r || r->fps <= 0) return 0;
    return (r->frames + r->fps - 1) / r->fps;
}

const unsigned char *avi_frame(AviReader *r, int index, int *len_out)
{
    if (!r || index < 0 || index >= r->frames) return NULL;
    if (len_out) *len_out = r->len[index];
    return r->data + r->off[index];
}

int avi_probe(const char *path, int *w, int *h, int *fps, int *frames)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    unsigned char buf[4096];
    int n = (int)read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 64) return -1;
    int W = 0, H = 0, F = 15, N = 0;
    for (int i = 0; i + 40 < n; i++) {
        if (!memcmp(buf + i, "strf", 4)) {
            W = (int)get_u32(buf + i + 12);
            H = (int)get_u32(buf + i + 16);
            break;
        }
    }
    for (int i = 0; i + 56 < n; i++) {
        if (!memcmp(buf + i, "strh", 4) && !memcmp(buf + i + 8, "vids", 4)) {
            uint32_t scale = get_u32(buf + i + 8 + 20);
            uint32_t rate = get_u32(buf + i + 8 + 24);
            if (scale > 0) { F = (int)(rate / scale); if (F <= 0 || F > 60) F = 15; }
            break;
        }
    }
    /* avih: 四个字节 'avih' + 4 字节长度，然后才是 MainAVIHeader。
     * dwTotalFrames 位于结构体偏移 16，也就是 'avih' 之后 8+16 处。 */
    for (int i = 0; i + 32 < n; i++) {
        if (!memcmp(buf + i, "avih", 4)) { N = (int)get_u32(buf + i + 8 + 16); break; }
    }
    if (w) *w = W;
    if (h) *h = H;
    if (fps) *fps = F;
    if (frames) *frames = N;
    return 0;
}
