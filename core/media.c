/*
 * media.c — 媒体库实现
 *
 * 目录结构（默认 /root/shixi，可用环境变量 SHIXI_ROOT 覆盖）：
 *   <root>/photo/IMG_YYYYMMDD_HHMMSS.jpg
 *   <root>/video/VID_YYYYMMDD_HHMMSS.avi
 *   <root>/.cache/<同名>.tmb          缩略图磁盘缓存
 *   <root>/tmp/                       录像临时文件
 */
#include "media.h"
#include "image.h"
#include "avi.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <errno.h>

static char g_root[256] = "/root/shixi";
static char g_photo[300], g_video[300], g_cache[300], g_tmp[300];

static void join(char *dst, int n, const char *a, const char *b)
{
    snprintf(dst, n, "%s/%s", a, b);
}

static int ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

int media_init(void)
{
    const char *env = getenv("SHIXI_ROOT");
    if (env && *env) snprintf(g_root, sizeof(g_root), "%s", env);
    join(g_photo, sizeof(g_photo), g_root, "photo");
    join(g_video, sizeof(g_video), g_root, "video");
    join(g_cache, sizeof(g_cache), g_root, ".cache");
    join(g_tmp,   sizeof(g_tmp),   g_root, "tmp");
    if (ensure_dir(g_root) || ensure_dir(g_photo) || ensure_dir(g_video) ||
        ensure_dir(g_cache) || ensure_dir(g_tmp)) {
        fprintf(stderr, "media: 无法创建目录 %s\n", g_root);
        return -1;
    }
    return 0;
}

const char *media_root(void)      { return g_root; }
const char *media_photo_dir(void) { return g_photo; }
const char *media_video_dir(void) { return g_video; }
const char *media_tmp_dir(void)   { return g_tmp; }

/* ---------------- 命名 ---------------- */
static void stamp(char *buf, int n)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(buf, n, "%04d%02d%02d_%02d%02d%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec);
}

int media_new_path(int type, char *path, int pathlen)
{
    char ts[32];
    stamp(ts, sizeof(ts));
    const char *dir = (type == MEDIA_VIDEO) ? g_video : g_photo;
    const char *pre = (type == MEDIA_VIDEO) ? "VID" : "IMG";
    const char *ext = (type == MEDIA_VIDEO) ? "avi" : "jpg";
    /* 同秒内重复时加序号 */
    for (int i = 0; i < 100; i++) {
        if (i == 0) snprintf(path, pathlen, "%s/%s_%s.%s", dir, pre, ts, ext);
        else        snprintf(path, pathlen, "%s/%s_%s_%02d.%s", dir, pre, ts, i, ext);
        if (access(path, F_OK) != 0) return 0;
    }
    return -1;
}

int media_new_temp_path(char *path, int pathlen)
{
    char ts[32];
    stamp(ts, sizeof(ts));
    snprintf(path, pathlen, "%s/rec_%s.avi", g_tmp, ts);
    return 0;
}

int media_commit_video(const char *tmp_path, char *final_path, int pathlen)
{
    if (media_new_path(MEDIA_VIDEO, final_path, pathlen) != 0) return -1;
    if (rename(tmp_path, final_path) != 0) return -1;
    return 0;
}

/* ---------------- 扫描 ---------------- */
static int has_ext(const char *name, const char *ext)
{
    size_t ln = strlen(name), le = strlen(ext);
    if (ln <= le) return 0;
    return strcasecmp(name + ln - le, ext) == 0;
}

static int cmp_items_desc(const void *a, const void *b)
{
    const MediaItem *x = (const MediaItem *)a, *y = (const MediaItem *)b;
    /* 按修改时间倒序；同一秒内再按文件名倒序（文件名里带时间戳） */
    if (x->mtime != y->mtime) return y->mtime - x->mtime;
    return -strcmp(x->name, y->name);
}

static int scan_dir(const char *dir, int type, MediaItem *items, int n, int max)
{
    DIR *dp = opendir(dir);
    if (!dp) return n;
    struct dirent *ep;
    while ((ep = readdir(dp)) != NULL && n < max) {
        if (ep->d_name[0] == '.') continue;
        int is_photo = (type == MEDIA_PHOTO) && (has_ext(ep->d_name, ".jpg") || has_ext(ep->d_name, ".ppm"));
        int is_video = (type == MEDIA_VIDEO) && has_ext(ep->d_name, ".avi");
        if (!is_photo && !is_video) continue;
        MediaItem *it = &items[n];
        memset(it, 0, sizeof(*it));
        /* 文件名不会超过 63 字节（时间戳命名），这里限长防溢出 */
        size_t nlen = strlen(ep->d_name);
        if (nlen > sizeof(it->name) - 1) nlen = sizeof(it->name) - 1;
        memcpy(it->name, ep->d_name, nlen);
        it->name[nlen] = 0;
        snprintf(it->path, sizeof(it->path), "%s/%s", dir, ep->d_name);
        struct stat st;
        if (stat(it->path, &st) != 0) continue;
        it->type = type;
        it->mtime = (int)st.st_mtime;
        it->size_kb = (int)(st.st_size / 1024);
        if (is_video) {
            int w = 0, h = 0, fps = 0, frames = 0;
            if (avi_probe(it->path, &w, &h, &fps, &frames) == 0) {
                it->width = w; it->height = h; it->frames = frames;
                it->duration_s = (fps > 0) ? (frames + fps - 1) / fps : 0;
            }
        }
        n++;
    }
    closedir(dp);
    return n;
}

int media_scan(MediaItem *items, int max_items)
{
    if (!items || max_items <= 0) return 0;
    int n = 0;
    n = scan_dir(g_photo, MEDIA_PHOTO, items, n, max_items);
    n = scan_dir(g_video, MEDIA_VIDEO, items, n, max_items);
    if (n > 1) qsort(items, n, sizeof(MediaItem), cmp_items_desc);
    return n;
}

int media_count_type(const MediaItem *items, int n, int type)
{
    int c = 0;
    for (int i = 0; i < n; i++) if (items[i].type == type) c++;
    return c;
}

/* ---------------- 删除 ---------------- */
/* 拼接 "<cache>/<name>.tmb"，逐段限长，避免任何截断风险 */
static void cache_path_for(const char *name, char *out, int n)
{
    int l = snprintf(out, n, "%s/", g_cache);
    if (l < 0 || l >= n) { if (n > 0) out[n - 1] = 0; return; }
    size_t room = (size_t)(n - l - 1);
    size_t nl = strlen(name);
    if (nl > room) nl = room;
    memcpy(out + l, name, nl);
    l += (int)nl;
    room = (size_t)(n - l - 1);
    const char *sfx = ".tmb";
    size_t sl = strlen(sfx);
    if (sl > room) sl = room;
    memcpy(out + l, sfx, sl);
    out[l + (int)sl] = 0;
}

int media_delete(const MediaItem *it)
{
    if (!it) return -1;
    char cp[340];
    cache_path_for(it->name, cp, sizeof(cp));
    unlink(cp);
    if (unlink(it->path) != 0) return -1;
    media_thumb_cache_clear();
    return 0;
}

/* ---------------- 缩略图 ---------------- */
#define TMB_MAGIC 0x53484D42u
typedef struct {
    uint32_t magic;
    int w, h, mtime, size;
} TmbHeader;

#define MEM_CACHE_MAX 24
typedef struct {
    char path[320];
    int  mtime;
    Surface *surf;
} ThumbMemEnt;
static ThumbMemEnt thumb_mem[MEM_CACHE_MAX];
static int thumb_mem_n = 0;

void media_thumb_cache_clear(void)
{
    for (int i = 0; i < thumb_mem_n; i++) {
        if (thumb_mem[i].surf) gfx_surface_free(thumb_mem[i].surf);
    }
    thumb_mem_n = 0;
}

void media_thumb_invalidate(const MediaItem *it)
{
    if (!it) return;
    for (int i = 0; i < thumb_mem_n; i++) {
        if (!strcmp(thumb_mem[i].path, it->path)) {
            if (thumb_mem[i].surf) gfx_surface_free(thumb_mem[i].surf);
            thumb_mem[i] = thumb_mem[thumb_mem_n - 1];
            thumb_mem_n--;
            i--;
        }
    }
}

/* 从磁盘缓存加载；成功返回 Surface */
static Surface *thumb_load_disk(const char *cache_path, int mtime, int size)
{
    int len = 0;
    unsigned char *data = file_read_all(cache_path, &len);
    if (!data) return NULL;
    if (len < (int)sizeof(TmbHeader)) { free(data); return NULL; }
    TmbHeader h;
    memcpy(&h, data, sizeof(h));
    if (h.magic != TMB_MAGIC || h.mtime != mtime || h.size != size ||
        h.w != THUMB_W || h.h != THUMB_H ||
        len < (int)(sizeof(TmbHeader) + (size_t)h.w * h.h * 4)) {
        free(data);
        return NULL;
    }
    Surface *s = gfx_surface_new(h.w, h.h);
    if (!s) { free(data); return NULL; }
    memcpy(s->px, data + sizeof(TmbHeader), (size_t)h.w * h.h * 4);
    free(data);
    return s;
}

static void thumb_save_disk(const char *cache_path, const Surface *s, int mtime, int size)
{
    TmbHeader h;
    h.magic = TMB_MAGIC;
    h.w = s->w; h.h = s->h; h.mtime = mtime; h.size = size;
    int total = (int)sizeof(TmbHeader) + s->w * s->h * 4;
    unsigned char *buf = (unsigned char *)malloc(total);
    if (!buf) return;
    memcpy(buf, &h, sizeof(h));
    for (int y = 0; y < s->h; y++)
        memcpy(buf + sizeof(TmbHeader) + (size_t)y * s->w * 4,
               s->px + (size_t)y * s->stride, (size_t)s->w * 4);
    file_write_all(cache_path, buf, total);
    free(buf);
}

/* 生成缩略图：照片解码缩放；视频取第一帧 */
static Surface *thumb_generate(const MediaItem *it)
{
    Surface *full = NULL;
    if (it->type == MEDIA_VIDEO) {
        AviReader *r = avi_open(it->path);
        if (!r) return NULL;
        int len = 0;
        const unsigned char *jpeg = avi_frame(r, 0, &len);
        if (jpeg) full = image_load_mem(jpeg, len);
        avi_close(r);
    } else {
        full = image_load_file(it->path);
    }
    if (!full) return NULL;
    Surface *t = image_thumbnail(full, THUMB_W, THUMB_H);
    gfx_surface_free(full);
    return t;
}

Surface *media_thumb(const MediaItem *it)
{
    if (!it) return NULL;
    /* 内存缓存 */
    for (int i = 0; i < thumb_mem_n; i++) {
        if (!strcmp(thumb_mem[i].path, it->path)) {
            if (thumb_mem[i].mtime == it->mtime) return thumb_mem[i].surf;
            /* 文件已更新：丢弃旧缓存 */
            gfx_surface_free(thumb_mem[i].surf);
            thumb_mem[i] = thumb_mem[thumb_mem_n - 1];
            thumb_mem_n--;
            break;
        }
    }
    char cp[340];
    cache_path_for(it->name, cp, sizeof(cp));
    Surface *s = thumb_load_disk(cp, it->mtime, it->size_kb);
    if (!s) {
        s = thumb_generate(it);
        if (s) thumb_save_disk(cp, s, it->mtime, it->size_kb);
    }
    if (!s) return NULL;
    if (thumb_mem_n >= MEM_CACHE_MAX) {
        /* 简单淘汰：丢掉最早的一个 */
        gfx_surface_free(thumb_mem[0].surf);
        memmove(&thumb_mem[0], &thumb_mem[1], sizeof(ThumbMemEnt) * (MEM_CACHE_MAX - 1));
        thumb_mem_n--;
    }
    snprintf(thumb_mem[thumb_mem_n].path, sizeof(thumb_mem[thumb_mem_n].path), "%s", it->path);
    thumb_mem[thumb_mem_n].mtime = it->mtime;
    thumb_mem[thumb_mem_n].surf = s;
    thumb_mem_n++;
    return s;
}

/* ---------------- 空间 ---------------- */
static void statvfs_root(struct statvfs *st)
{
    memset(st, 0, sizeof(*st));
    statvfs(g_root, st);
}

int media_free_mb(void)
{
    struct statvfs st;
    statvfs_root(&st);
    return (int)((double)st.f_bavail * st.f_frsize / (1024 * 1024));
}

int media_used_mb(void)
{
    struct statvfs st;
    statvfs_root(&st);
    return (int)((double)(st.f_blocks - st.f_bfree) * st.f_frsize / (1024 * 1024));
}

void media_cleanup_tmp(void)
{
    DIR *dp = opendir(g_tmp);
    if (!dp) return;
    struct dirent *ep;
    while ((ep = readdir(dp)) != NULL) {
        if (ep->d_name[0] == '.') continue;
        char p[340];
        int l = snprintf(p, sizeof(p), "%s/", g_tmp);
        if (l > 0 && l < (int)sizeof(p)) {
            size_t room = sizeof(p) - (size_t)l - 1;
            size_t nl = strlen(ep->d_name);
            if (nl > room) nl = room;
            memcpy(p + l, ep->d_name, nl);
            p[l + (int)nl] = 0;
            unlink(p);
        }
    }
    closedir(dp);
}
