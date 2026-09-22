/*
 * avi.h — MJPEG AVI 封装/解封装（录像与回放）
 *
 * 写：边录边写（逐帧写入 '00dc'），结束时回填头部与索引，得到一个标准 MJPEG AVI，
 *     电脑上的播放器（VLC/暴风）与板上的 mplayer 都能直接播放。
 * 读：整文件读入内存并扫描 'movi' 得到帧索引，供内置播放器逐帧解码显示。
 */
#ifndef SHIXI_AVI_H
#define SHIXI_AVI_H

typedef struct AviWriter AviWriter;
typedef struct AviReader AviReader;

/* ---------------- 写 ---------------- */
AviWriter *avi_writer_open(const char *path, int width, int height, int fps);
int  avi_writer_add(AviWriter *w, const void *jpeg, int len);
int  avi_writer_frames(AviWriter *w);
int  avi_writer_bytes(AviWriter *w);
/* 结束前调整帧率：实际采集帧率可能低于目标值，写真实值播放速度才正确 */
void avi_writer_set_fps(AviWriter *w, int fps);
/* 回填头部与索引并关闭；abort=1 时直接删除文件（录制被取消） */
int  avi_writer_close(AviWriter *w, int abort);

/* ---------------- 读 ---------------- */
AviReader *avi_open(const char *path);
void avi_close(AviReader *r);
int  avi_width(AviReader *r);
int  avi_height(AviReader *r);
int  avi_fps(AviReader *r);
int  avi_frame_count(AviReader *r);
int  avi_duration_s(AviReader *r);
/* 取第 index 帧的 JPEG 数据（顺序访问时开销为 0），返回指针与长度 */
const unsigned char *avi_frame(AviReader *r, int index, int *len_out);

/* 只读文件头，快速获取视频信息（相册列表用，不加载整文件） */
int  avi_probe(const char *path, int *w, int *h, int *fps, int *frames);

#endif /* SHIXI_AVI_H */
