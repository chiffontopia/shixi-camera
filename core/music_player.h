/*
 * music_player.h —— 音乐播放后端（mplayer -slave + 命名管道）
 *
 * 界面层只调用这里的接口：播放/暂停/上一首/下一首/音量/进度，
 * 不直接操作 mplayer 进程和管道。
 */
#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

#define MP_MAX_TRACKS 64   /* 最多收录的曲目数 */
#define MP_NAME_MAX   256  /* 单个文件名缓冲长度（含结尾 0，够放 NAME_MAX） */

/*
 * 扫描 dir 下的 *.mp3、创建并打开命令管道 fifo_path。
 * 成功（即使一首 mp3 都没有）返回 0，失败返回 -1 并打印原因。
 */
int MP_Init(const char *dir, const char *fifo_path);

/* 停掉播放器并关闭管道，可重复调用 */
void MP_Deinit(void);

int MP_TrackCount(void);
/* 第 idx 首的文件名（不含目录）；越界返回空串 */
const char *MP_TrackName(int idx);
/* 当前曲目下标；没有曲目时为 0 */
int MP_Index(void);

/* 播放器进程是否还活着（自然播完、被外部 kill 都算不活） */
int MP_Playing(void);
/* 是否处于暂停状态（SIGSTOP） */
int MP_Paused(void);

/* 切到第 idx 首并从头播放 */
void MP_Play(int idx);
/* 播放/暂停切换；当前没有播放器时从头播放当前曲目 */
void MP_TogglePause(void);
/* 停止播放：收掉 mplayer 并把进度归零，但**保留后端可用**（还能再 MP_Play）。
 * 与 MP_Deinit 的区别是后者会关掉命令管道，之后就不能再播了。 */
void MP_Stop(void);
void MP_Next(void);
void MP_Prev(void);

/* 音量 0~100（绝对设置，越界自动夹住） */
void MP_SetVolume(int vol);
int MP_Volume(void);

/* 跳到绝对秒数（0 ~ 总时长） */
void MP_Seek(int sec);
/* 当前播放位置（秒），未播放时为 0 */
int MP_Position(void);
/* 当前曲目总时长（秒），未知时为 0 */
int MP_Duration(void);

/* 由 LVGL 定时器周期性调用（建议 200ms）：收 mplayer 应答、播完自动下一首 */
void MP_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_PLAYER_H */
