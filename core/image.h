/*
 * image.h — 图片解码/编码/缩放（基于 stb_image，全内存接口）
 */
#ifndef SHIXI_IMAGE_H
#define SHIXI_IMAGE_H

#include "gfx.h"

/* 文件 <-> 内存 */
unsigned char *file_read_all(const char *path, int *size_out);
int file_write_all(const char *path, const void *data, int size);

/* 解码：支持 jpg / bmp / png（stb） */
Surface *image_load_file(const char *path);
Surface *image_load_mem(const unsigned char *data, int len);

/* 高质量缩放（双线性）与缩略图（等比裁剪居中到 w x h） */
Surface *image_scale(const Surface *src, int w, int h);
Surface *image_thumbnail(const Surface *src, int w, int h);

/* 另存为 JPEG（quality 1..100） */
int image_save_jpeg(const char *path, const Surface *s, int quality);
/* 编码成内存中的 JPEG（调用方 free 返回的缓冲区） */
int image_encode_jpeg_mem(const Surface *s, int quality, unsigned char **out, int *out_len);

#endif /* SHIXI_IMAGE_H */
