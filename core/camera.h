/*
 * camera.h — 摄像头采集（V4L2）+ 无摄像头时的演示信号源
 *
 * - 自动扫描 /dev/video0..15，挑选第一个支持 Video Capture 的设备（UVC 摄像头）
 * - 优先协商 MJPEG（帧可直接存盘/录像），不支持则用 YUYV 并自行转 RGB
 * - 采集线程负责解码，界面线程只做缩放贴图，避免卡界面
 * - 没有摄像头时启用"演示模式"（动态测试画面），保证 UI 可正常演示
 */
#ifndef SHIXI_CAMERA_H
#define SHIXI_CAMERA_H

#include "gfx.h"

typedef enum {
    CAM_SRC_NONE = 0,
    CAM_SRC_V4L2,
    CAM_SRC_TESTPATTERN
} CamSource;

#define CAM_FRAME_W 640
#define CAM_FRAME_H 480

int  camera_open(void);
void camera_close(void);
/*
 * 重新扫描摄像头。演示模式下如果插上了摄像头，会平滑切换到真实画面。
 * 返回 1 表示刚刚切换成功，0 表示无需切换，-1 表示没有找到。
 */
int  camera_rescan(void);
int  camera_is_open(void);
CamSource camera_source(void);
const char *camera_device_path(void);     /* 如 /dev/video7；演示模式返回 "-" */
int  camera_fps(void);                    /* 实测帧率 */
int  camera_pixel_format(void);           /* V4L2_PIX_FMT_* 或 0 */

/*
 * 取最新预览帧（RGB，640x480）。
 * 返回值可直接用于 gfx_blit_*；seq 变化表示是新的一帧。
 * 返回 NULL 表示暂时没有可用帧。
 */
const Surface *camera_preview(uint64_t *seq_out);

/*
 * 取最新一帧的 JPEG 数据（供保存照片/录像使用）。
 * 数据被拷贝到调用方提供的 buffer（需 >= 400KB），返回实际长度，<0 表示失败。
 */
int camera_latest_jpeg(unsigned char *out, int out_cap);

/* 采集参数：把上次成功帧的 JPEG 直接交出（录像用），内部拷贝，线程安全 */
int camera_latest_jpeg_len(void);
/* 最近一帧 JPEG 的年龄（毫秒） */
int camera_jpeg_age_ms(void);

/* 供界面显示状态：V4L2 模式下已经多久没收到帧（毫秒）；非 V4L2 返回 0 */
int camera_no_signal_ms(void);

/* 待上报事件（采集线程置位，界面取走并提示用户） */
typedef enum {
    CAM_EV_NONE = 0,
    CAM_EV_FELL_BACK,   /* 摄像头无响应，已退回演示画面 */
    CAM_EV_ACQUIRED     /* 刚识别到摄像头并切换成功 */
} CamEvent;
int camera_take_event(void);

#endif /* SHIXI_CAMERA_H */
