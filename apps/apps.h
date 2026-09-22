/*
 * apps.h — 应用接口定义
 *
 * 每个应用实现 enter/leave/frame/event 四个回调；main.c 负责调度与切换动画。
 */
#ifndef SHIXI_APPS_H
#define SHIXI_APPS_H

#include "gfx.h"
#include "input.h"
#include "ui.h"

typedef struct {
    const char *title;      /* 中文名 */
    const char *en;         /* 英文小标 */
    int   icon;             /* AppIconId */
    int   needs_camera;     /* 进入前是否需要打开摄像头 */
    void (*enter)(void);
    void (*leave)(void);
    void (*frame)(uint64_t t_ms);
    int  (*event)(const UiEvent *e);   /* 返回 1 表示请求回到桌面 */
} AppDef;

extern const AppDef g_app_camera;
extern const AppDef g_app_video;
extern const AppDef g_app_gallery;
extern const AppDef g_app_chat;
extern const AppDef g_app_brick;
extern const AppDef g_app_music;

/* 应用之间跳转（由 main.c 实现） */
void app_open(int app_index);                 /* 打开某个应用 */
void app_open_gallery_view(int media_index);  /* 直接打开图库并查看某一张 */
void app_go_home(void);
int  app_camera_prepare(void);                /* 惰性打开摄像头，返回 0 成功 */

/* 应用索引 */
enum { APP_CAMERA = 0, APP_VIDEO, APP_GALLERY, APP_CHAT, APP_BRICK, APP_MUSIC, APP_COUNT };
extern const AppDef *g_apps[APP_COUNT];

#endif /* SHIXI_APPS_H */
