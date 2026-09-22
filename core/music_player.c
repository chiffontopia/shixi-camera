/*
 * music_player.c —— 音乐播放后端：mplayer(-slave) + 命名管道
 *
 * 来源：原 LVGL 项目（`~/workspace/music_player.c`）的**原样搬运**，本身与 UI 无关，
 * 界面在 `apps/app_music.c`。相对原版**只有两处改动**：
 *   1. 给 mplayer 的曲目参数改成绝对路径（`music_dir/文件名`）——shixi 的工作目录是
 *      /root/shixi，而 mp3 通常在 /root，传相对文件名会找不到文件。
 *   2. 新增 `MP_Stop()`：收掉播放器并归零进度，但保留命令管道——「停止」按钮需要它，
 *      而 `MP_Deinit()` 是终态的（关了管道就不能再播）。
 *   3. exec 前关掉继承来的 fd：板上实测 mplayer 会继承 shixi 的 /dev/fb0、触摸设备、
 *      调试通道**和 flock 单实例锁**——残留的播放器会替我们攥着锁，导致 shixi 被
 *      SIGKILL 后下次启动报「程序已在运行」。
 * 其余逻辑（含下面的 mplayer 1.0rc2 行为记录）一字未动。
 *
 * 驱动点：`MP_Poll()` 由 `main.c` 的主循环按 200ms 节流调用（原版挂在 LVGL 定时器上），
 * 所以**离开音乐应用后仍然继续收应答、播完自动下一首**。
 *
 * 依赖的 mplayer 1.0rc2 行为（源码核对 + 开发板实测，video.c 的验证记录）：
 *   1. 启动：fork/exec 出 mplayer，-slave 让它读管道命令，-input file=<fifo>
 *      指定管道。父进程以 O_RDWR 持有管道：既保证 mplayer 不会读到 EOF（读到
 *      EOF 就不再收命令），也避免上次残留的命令被新播放器执行。
 *   2. 暂停/继续只能用 SIGSTOP/SIGCONT：rc2 的 "pause" 只能单向停住，之后
 *      再发 pause / pause 0 / pause 1 都恢复不了。
 *   3. 音量用绝对设置 "volume <0~100> 1"。rc2 的 "volume ±N" 只走
 *      mixer_incvolume/decvolume，N 被忽略，每发一次只动一个 volstep（默认 3），
 *      所以它没法表达任意音量；带 abs 参数的那条路是 mixer_setvolume，能精确设值。
 *   4. 进度跳转用 "seek <秒> 2"（第二个参数 2 = 绝对秒数）。
 *   5. 进度读取：周期性写 "get_time_pos"/"get_time_length"，mplayer 把
 *      "ANS_TIME_POSITION="/"ANS_LENGTH=" 打到 stdout，并且每条都 fflush，
 *      所以把子进程 stdout 接到管道里非阻塞读就行。-quiet 只关状态行，
 *      不影响这些应答。
 *   6. 文件播完 mplayer 自己退出（不带 -idle），waitpid 检测到就自动切下一首。
 *
 * 进度时间基准：pos_base_ms 是某时刻的真实位置，pos_base_tick 是该时刻；
 * 正在播放且未暂停时，位置 = base + 已经过的时间。收到 mplayer 应答后会把
 * base 校正到真实值，所以进度条既平滑又不会长期漂移；即使应答收不到
 * （例如 stdout 被 mplayer 换成别的行为），也能按时间走下去，不至于卡死不动。
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "music_player.h"

#define MP_PATH_MAX      256   /* 目录/管道路径长度 */
#define MP_VOL_DEFAULT   60    /* 启动音量 */
#define MP_QUERY_MS      500   /* 向 mplayer 查询进度的间隔 */
#define MP_ANSWER_MAX    128   /* 单行应答的最大长度 */
#define MP_QUIT_WAIT_MS  1000  /* 退出时等播放器自己结束的时间 */
#define MP_MIN_TRACK_MS  1000  /* 起播后不足这么久就退出，视为起播失败，不再自动下一首 */

/* ---------------- 全局状态 ---------------- */
static char  music_dir[MP_PATH_MAX];
static char  fifo_path[MP_PATH_MAX];
static char  input_opt[MP_PATH_MAX + 8];  /* mplayer 的 -input file=... 参数 */

static char  tracks[MP_MAX_TRACKS][MP_NAME_MAX];
static int   track_count;

static int   fd_fifo = -1;   /* 命令管道：写命令给 mplayer */
static int   fd_out = -1;    /* mplayer 的 stdout：读 ANS_ 应答 */
static pid_t player = -1;    /* 播放器进程号 */
static int   paused;         /* 1 = 已 SIGSTOP */
static int   cur_index;          /* 当前曲目下标 */
static int   volume = MP_VOL_DEFAULT;
static int   vol_synced;     /* 本曲目的音量是否已经真正设进播放器 */
static int   finishing;      /* 正在退出，播完也不要再自动下一首 */

static long long track_start_ms;  /* 本曲目起播时刻（判断是否起播失败） */
static long long pos_base_ms;     /* 位置基准 */
static long long pos_base_tick;   /* 位置基准对应的时刻 */
static long long duration_ms;     /* 总时长，0 = 未知 */
static long long last_query_ms;   /* 上次查询时刻 */

/* stdout 行缓冲：mplayer 的输出按行解析，只认 ANS_ 开头的行 */
static char   answer[MP_ANSWER_MAX];
static size_t answer_len;

/* ---------------- 基础工具 ---------------- */
static long long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int ends_with_mp3(const char *name)
{
	size_t n = strlen(name);

	return n > 4 && strcasecmp(name + n - 4, ".mp3") == 0;
}

static int name_cmp(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

/* ---------------- 命令与应答 ---------------- */
static int Send_Cmd(const char *cmd)
{
	size_t len = strlen(cmd);

	if (fd_fifo < 0)
		return -1;
	if (write(fd_fifo, cmd, len) != (ssize_t)len) {
		fprintf(stderr, "发送命令失败: %s (%s)\n", cmd, strerror(errno));
		return -1;
	}
	return 0;
}

/* 把音量真正设进播放器（绝对设置） */
static void Apply_Volume(void)
{
	char cmd[32];

	snprintf(cmd, sizeof cmd, "volume %d 1\n", volume);
	if (Send_Cmd(cmd) == 0)
		vol_synced = 1;
}

static void Line_Ready(void)
{
	answer[answer_len] = '\0';

	if (strncmp(answer, "ANS_TIME_POSITION=", 18) == 0) {
		double sec = atof(answer + 18);

		if (sec < 0)
			return;
		pos_base_ms = (long long)(sec * 1000);
		pos_base_tick = now_ms();
		if (!vol_synced && !paused)
			Apply_Volume();   /* 播放器已就绪，这时设音量才不会被丢掉 */
	} else if (strncmp(answer, "ANS_LENGTH=", 11) == 0) {
		double sec = atof(answer + 11);

		if (sec > 0)
			duration_ms = (long long)(sec * 1000);
	}
}

static void Feed_Answer(const char *buf, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		char c = buf[i];

		if (c == '\n' || c == '\r') {
			if (answer_len > 0) {
				Line_Ready();
				answer_len = 0;
			}
			continue;
		}
		/* 超长行（mplayer 的普通日志）直接丢掉，只留最后一段 */
		if (answer_len < sizeof answer - 1)
			answer[answer_len++] = c;
		else
			answer_len = 0;
	}
}

/* 读完 stdout 上已有的数据：既解析应答，也防止管道被 mplayer 写满而卡住播放 */
static void Drain_Answers(void)
{
	char buf[256];
	ssize_t n;

	if (fd_out < 0)
		return;
	while ((n = read(fd_out, buf, sizeof buf)) > 0)
		Feed_Answer(buf, (size_t)n);
}

/* ---------------- 播放器进程 ---------------- */
static void Close_Out(void)
{
	if (fd_out >= 0) {
		close(fd_out);
		fd_out = -1;
	}
	answer_len = 0;
}

static int Player_Start(void)
{
	int fds[2];
	pid_t pid;

	if (pipe(fds) != 0) {
		fprintf(stderr, "pipe 失败: %s\n", strerror(errno));
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork 失败: %s\n", strerror(errno));
		close(fds[0]);
		close(fds[1]);
		return -1;
	}

	if (pid == 0) {
		/* 子进程：stdout 接管道（父进程用来读 ANS_ 应答），信号恢复默认 */
		close(fds[0]);
		if (dup2(fds[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(fds[1]);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		/* 关掉从父进程继承来的 fd（/dev/fb0、触摸、flock 单实例锁、调试通道…）：
		 * 既避免播放器无意持有这些设备，也避免它替我们一直攥着 shixi 的单实例锁
		 * —— 否则 shixi 被 SIGKILL 后残留的 mplayer 会让下次启动报"程序已在运行"。 */
		{
			long maxfd = sysconf(_SC_OPEN_MAX);

			if (maxfd < 0 || maxfd > 1024)
				maxfd = 1024;
			for (int fd = 3; fd < (int)maxfd; fd++)
				close(fd);
		}
		/* -vo null：mp3 带封面图时不要让 mplayer 去抢 fb0，屏幕归 shixi */
		/* 曲目传绝对路径：shixi 的工作目录是 /root/shixi，而 mp3 可能在 /root */
		{
			char full[MP_PATH_MAX * 2];

			snprintf(full, sizeof full, "%s/%s", music_dir, tracks[cur_index]);
			execlp("mplayer", "mplayer", "-slave", "-quiet",
			       "-vo", "null", "-input", input_opt,
			       full, (char *)NULL);
			perror("execlp mplayer");
		}
		_exit(127);
	}

	close(fds[1]);
	fd_out = fds[0];
	fcntl(fd_out, F_SETFL, O_NONBLOCK);
	fcntl(fd_out, F_SETFD, FD_CLOEXEC);
	player = pid;
	return 0;
}

/* 停掉当前播放器，阻塞最多约 1s；幂等 */
static void Player_Stop(void)
{
	int status, i;

	if (player <= 0)
		return;

	if (paused) {                 /* 被 SIGSTOP 的进程收不到 quit，先恢复 */
		kill(player, SIGCONT);
		paused = 0;
	}
	Send_Cmd("quit\n");

	for (i = 0; i < MP_QUIT_WAIT_MS / 50; i++) {
		if (waitpid(player, &status, WNOHANG) == player)
			goto out;
		usleep(50 * 1000);
	}

	fprintf(stderr, "播放器无响应，强制结束\n");
	kill(player, SIGTERM);
	for (i = 0; i < 10; i++) {
		if (waitpid(player, &status, WNOHANG) == player)
			goto out;
		usleep(50 * 1000);
	}
	kill(player, SIGKILL);
	waitpid(player, &status, 0);

out:
	player = -1;
	Close_Out();
}

/* ---------------- 曲目表 ---------------- */
static void Scan_Tracks(void)
{
	DIR *dir;
	struct dirent *ent;

	track_count = 0;
	dir = opendir(music_dir);
	if (dir == NULL) {
		fprintf(stderr, "打开目录 %s 失败: %s\n", music_dir, strerror(errno));
		return;
	}
	while ((ent = readdir(dir)) != NULL) {
		if (track_count >= MP_MAX_TRACKS) {
			fprintf(stderr, "曲目超过 %d 首，只收录前 %d 首\n",
				MP_MAX_TRACKS, MP_MAX_TRACKS);
			break;
		}
		if (!ends_with_mp3(ent->d_name))
			continue;
		snprintf(tracks[track_count], sizeof tracks[0], "%s", ent->d_name);
		track_count++;
	}
	closedir(dir);

	qsort(tracks, (size_t)track_count, sizeof tracks[0], name_cmp);
	printf("在 %s 找到 %d 首 mp3\n", music_dir, track_count);
}

/* ---------------- 对外接口 ---------------- */
int MP_Init(const char *dir, const char *fifo)
{
	struct stat st;
	char buf[256];
	struct pollfd pfd;

	snprintf(music_dir, sizeof music_dir, "%s", dir ? dir : "/root");
	snprintf(fifo_path, sizeof fifo_path, "%s", fifo ? fifo : "/root/mp_fifo");
	snprintf(input_opt, sizeof input_opt, "file=%s", fifo_path);

	Scan_Tracks();

	/* 管道文件：不存在才建，权限 0666（mplayer 常以 root 运行，普通用户也能写） */
	if (stat(fifo_path, &st) != 0 &&
	    mkfifo(fifo_path, 0666) != 0 && errno != EEXIST) {
		fprintf(stderr, "创建管道 %s 失败: %s\n", fifo_path, strerror(errno));
		return -1;
	}
	fd_fifo = open(fifo_path, O_RDWR | O_NONBLOCK);
	if (fd_fifo < 0) {
		fprintf(stderr, "打开管道 %s 失败: %s\n", fifo_path, strerror(errno));
		return -1;
	}

	/* 丢掉上次运行残留在管道里的命令，别喂给新播放器 */
	pfd.fd = fd_fifo;
	pfd.events = POLLIN;
	while (poll(&pfd, 1, 0) > 0) {
		if (read(fd_fifo, buf, sizeof buf) <= 0)
			break;
	}
	return 0;
}

void MP_Deinit(void)
{
	finishing = 1;
	Player_Stop();
	if (fd_fifo >= 0) {
		close(fd_fifo);
		fd_fifo = -1;
	}
}

int MP_TrackCount(void)
{
	return track_count;
}

const char *MP_TrackName(int idx)
{
	if (idx < 0 || idx >= track_count)
		return "";
	return tracks[idx];
}

int MP_Index(void)
{
	return cur_index;
}

int MP_Playing(void)
{
	return player > 0;
}

int MP_Paused(void)
{
	return paused;
}

void MP_Play(int idx)
{
	if (track_count <= 0)
		return;
	if (idx < 0 || idx >= track_count)
		idx = 0;

	Player_Stop();                 /* 先收掉上一个进程，再起新的 */

	cur_index = idx;
	paused = 0;
	vol_synced = 0;
	pos_base_ms = 0;
	pos_base_tick = now_ms();
	duration_ms = 0;
	last_query_ms = 0;
	answer_len = 0;

	if (Player_Start() < 0)
		return;

	track_start_ms = now_ms();
	printf("播放第 %d 首: %s\n", cur_index + 1, tracks[cur_index]);
	Apply_Volume();                /* 先试一次；万一这时声道还没就绪，
	                                * 收到第一个应答后会再设一次 */
}

void MP_TogglePause(void)
{
	if (player <= 0) {             /* 播完了/没播过：从头开始播当前曲目 */
		MP_Play(cur_index);
		return;
	}
	if (paused) {
		if (kill(player, SIGCONT) == 0) {
			paused = 0;
			pos_base_tick = now_ms();   /* 暂停这段时间不计入进度 */
		}
	} else if (kill(player, SIGSTOP) == 0) {
		pos_base_ms = (long long)MP_Position() * 1000;
		pos_base_tick = now_ms();
		paused = 1;
	}
}

void MP_Stop(void)
{
	Player_Stop();
	pos_base_ms = 0;
	pos_base_tick = now_ms();
	duration_ms = 0;
}

void MP_Next(void)
{
	if (track_count > 0)
		MP_Play((cur_index + 1) % track_count);
}

void MP_Prev(void)
{
	if (track_count > 0)
		MP_Play((cur_index + track_count - 1) % track_count);
}

void MP_SetVolume(int vol)
{
	if (vol < 0)
		vol = 0;
	if (vol > 100)
		vol = 100;
	volume = vol;
	if (player > 0 && vol_synced)
		Apply_Volume();
}

int MP_Volume(void)
{
	return volume;
}

void MP_Seek(int sec)
{
	char cmd[32];
	int total;

	if (player <= 0)
		return;
	if (sec < 0)
		sec = 0;
	if (duration_ms > 0) {
		total = (int)((duration_ms + 999) / 1000);
		if (total > 1 && sec >= total)
			sec = total - 1;   /* 跳到最末尾会立刻播完，留 1 秒 */
	}

	snprintf(cmd, sizeof cmd, "seek %d 2\n", sec);
	Send_Cmd(cmd);
	pos_base_ms = (long long)sec * 1000;
	pos_base_tick = now_ms();
}

int MP_Position(void)
{
	long long ms = pos_base_ms;

	if (player > 0 && !paused)
		ms += now_ms() - pos_base_tick;
	if (duration_ms > 0 && ms > duration_ms)
		ms = duration_ms;
	if (ms < 0)
		ms = 0;
	return (int)(ms / 1000);
}

int MP_Duration(void)
{
	return (int)(duration_ms / 1000);
}

void MP_Poll(void)
{
	int status;
	pid_t r;
	long long played;

	if (player <= 0)
		return;

	Drain_Answers();

	r = waitpid(player, &status, WNOHANG);
	if (r == player || (r < 0 && errno == ECHILD)) {
		played = now_ms() - track_start_ms;
		player = -1;
		paused = 0;
		Close_Out();
		printf("播放器已退出（第 %d 首，播放 %lld ms）\n", cur_index + 1, played);
		if (!finishing && track_count > 0 && played > MP_MIN_TRACK_MS)
			MP_Play((cur_index + 1) % track_count);   /* 播完自动下一首 */
		return;
	}

	if (!paused && now_ms() - last_query_ms >= MP_QUERY_MS) {
		last_query_ms = now_ms();
		Send_Cmd("get_time_pos\n");
		Send_Cmd("get_time_length\n");
	}
}
