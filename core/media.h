/*
 * media.h — 媒体库（照片 / 视频文件的扫描、命名、删除、缩略图缓存）
 */
#ifndef SHIXI_MEDIA_H
#define SHIXI_MEDIA_H

#include "gfx.h"

#define MEDIA_PHOTO 0
#define MEDIA_VIDEO 1

#define MEDIA_MAX_ITEMS 300
#define THUMB_W 186
#define THUMB_H 140

typedef struct {
    char path[320];
    char name[64];
    int  type;          /* MEDIA_PHOTO / MEDIA_VIDEO */
    int  mtime;
    int  size_kb;
    int  duration_s;    /* 视频时长（秒） */
    int  frames;
    int  width, height;
} MediaItem;

int  media_init(void);                       /* 创建目录结构 */
const char *media_root(void);
const char *media_photo_dir(void);
const char *media_video_dir(void);
const char *media_tmp_dir(void);

/* 生成新的文件名（写入 path）；type 决定前缀与扩展名 */
int  media_new_path(int type, char *path, int pathlen);
/* 生成录像用的临时文件路径（结束后改名到正式文件） */
int  media_new_temp_path(char *path, int pathlen);
/* 把临时录像文件转正（重命名），返回 0 成功 */
int  media_commit_video(const char *tmp_path, char *final_path, int pathlen);

/* 扫描媒体库：按时间倒序（新文件在前），返回条数 */
int  media_scan(MediaItem *items, int max_items);
int  media_count_type(const MediaItem *items, int n, int type);

/* 删除文件及其缓存 */
int  media_delete(const MediaItem *it);

/* 缩略图（带内存缓存 + 磁盘缓存），失败返回 NULL */
Surface *media_thumb(const MediaItem *it);
void media_thumb_cache_clear(void);
void media_thumb_invalidate(const MediaItem *it);

/* 空间信息 */
int  media_free_mb(void);
int  media_used_mb(void);
/* 清理录像临时目录里的残留文件 */
void media_cleanup_tmp(void);

#endif /* SHIXI_MEDIA_H */
