/*
 * ai.h — 板端 LLM 客户端（OpenAI 兼容 /v1/chat/completions）
 *
 * 与 UI 无关（同 music_player 的分工）：app_chat.c 只负责画面，这里负责
 * 起 curl、等结果、解析 JSON、维护最近几轮对话。
 *
 * 设计见 .scratch/board-desktop/design/11-ai-assistant-transport.md：
 *   · fork/exec 静态 curl；请求体走临时文件（命令行里没有 JSON）
 *   · 密钥走 curl 的 -K 配置文件（0600），不进 argv → ps 看不到
 *   · 主循环里 ai_poll() 轮询 waitpid(WNOHANG)，界面全程不阻塞
 *   · 超时 / 取消 / 离开界面都 kill 子进程，不留孤儿
 *   · 失败的轮次不写进历史 → 「重试」不会把同一句话记两遍
 *
 * 典型用法（每帧一次）：
 *     AiState st = ai_poll();
 *     if (st == AI_BUSY) 画等待态;
 *     else if (st == AI_OK)  add_msg(0, ai_reply());
 *     else if (st == AI_ERR) add_msg(2, ai_error()), 显示「重试」;
 */
#ifndef SHIXI_AI_H
#define SHIXI_AI_H

typedef enum {
    AI_IDLE = 0,   /* 空闲 */
    AI_BUSY,       /* 请求进行中 */
    AI_OK,         /* 刚拿到回复（只在那一帧返回一次，之后回到 AI_IDLE） */
    AI_ERR         /* 刚失败（同上，只报一次） */
} AiState;

/* 读配置、解析临时文件路径。可重复调用（改了 relay_* / ai.conf 后重新进界面即生效）。
 * 返回 1 = 配了 endpoint（联网模式），0 = 没配（调用方走演示模式）。 */
int  ai_init(void);
/* 取消在跑的请求并删掉临时文件（含那份带密钥的 curl 配置） */
void ai_deinit(void);

int         ai_configured(void);      /* 1 = 有 endpoint */
const char *ai_model(void);           /* 当前模型名，给顶栏显示 */

void    ai_ask(const char *text);     /* 发起一轮请求（正在跑则忽略） */
void    ai_cancel(void);              /* 中止在跑的请求 */
AiState ai_poll(void);                /* 每帧调一次 */
const char *ai_reply(void);           /* AI_OK 时有效 */
const char *ai_error(void);           /* AI_ERR 时有效 */
const char *ai_last_user(void);       /* 最近一次请求的用户原文（重试用） */
void    ai_history_clear(void);       /* 清空远端上下文（最近 N 轮） */

#endif /* SHIXI_AI_H */
