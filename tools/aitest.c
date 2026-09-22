/*
 * aitest.c — AI 链路自检工具（板端/主机都能跑，不开界面就能验证整条链路）
 *
 * 用途：AI 聊天气泡不出来时，先用它定位到底卡在哪一环
 *   （配置没读到 / 传输命令失败 / 网络不通 / 模型返回被截断 / 解析失败）。
 *
 * 用法：
 *   ./aitest                     用默认问题
 *   ./aitest "你好，介绍一下你自己"
 *   SHIXI_ROOT=/tmp/x ./aitest   换个工作目录（主机上调试用）
 *
 * 退出码：0 成功，1 配置/启动失败，2 请求失败，3 超时
 */
#include "ai.h"
#include "media.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *q = argc > 1 ? argv[1] : "用一句话介绍你自己";

    if (media_init() != 0) {
        printf("存储初始化失败（media_init）\n");
        return 1;
    }

    int net = ai_init();
    printf("配置：%s  模型：%s\n", net ? "已配置" : "未配置（没有 relay_url，也没配 transport）",
           ai_model());
    if (!net) {
        printf("提示：板子侧写 $SHIXI_ROOT/relay_url+relay_key（走宿主机 relay），\n");
        printf("      或在 $SHIXI_ROOT/ai.conf 里写 transport=/root/shixi/ai_bridge_board.sh\n");
        return 1;
    }

    printf("提问：%s\n", q);
    ai_ask(q);
    uint64_t t0 = now_ms();
    for (;;) {
        AiState st = ai_poll();
        if (st == AI_OK) {
            printf("\n回答（%.1f 秒）：\n%s\n", (now_ms() - t0) / 1000.0, ai_reply());
            return 0;
        }
        if (st == AI_ERR) {
            printf("\n失败（%.1f 秒）：\n%s\n", (now_ms() - t0) / 1000.0, ai_error());
            printf("\n排查：看 $SHIXI_ROOT/tmp/ai_req.json（发出去的请求）、\n");
            printf("      ai_status.txt / ai_err.txt（HTTP 码与传输命令输出）\n");
            return 2;
        }
        if (now_ms() - t0 > 40000) {
            printf("\n超时（40 秒）\n");
            ai_cancel();
            return 3;
        }
        usleep(100000);
        printf(".");
        fflush(stdout);
    }
}
