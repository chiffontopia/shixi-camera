/*
 * ai_parse_test.c — core/ai.c 的「回复解析 + 错误分类」门禁（主机程序，不参与板上构建）
 *
 * 为什么要这个测试：解析器是手写的（板上没有 JSON 库），而这块**错过一次**——
 * 第一版 obj_find_str() 忘了跨过 key 后面的冒号，于是 message.content 永远找不到，
 * 界面上显示「模型没有给出内容」，而实际上模型答得好好的（curl 日志里能看到）。
 * 这类错不会崩、只会静默给错结果，所以拿**真实抓包**钉住它。
 *
 * fixtures（tools/testdata/，都是从宿主机 CCX 转发服务抓下来的原样响应）：
 *   ai_ok.json     200，content 正常（中文），reasoning_content 是空串
 *   ai_quotes.json 200，content 里带转义引号（\"content\"），且 reasoning_content 非空
 *   ai_length.json 200，max_tokens 太小：content 空 + finish_reason=length
 *   ai_401.json    401，{"error":"字符串"}
 *   ai_400.json    400，{"error":{"message":"…"}}（对象形）
 *
 * 这里直接 #include "../core/ai.c" —— 为了够到 static 的解析函数与 classify()。
 * 用法：make ai-parse-test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../core/ai.h"
#include "../core/media.h"
#include "../core/util.h"

/* media 的路径查询在这里给桩：本测试不建目录、只读 fixtures */
const char *media_root(void)    { return "/tmp/ai-parse-test"; }
const char *media_tmp_dir(void) { return "/tmp/ai-parse-test"; }

#include "../core/ai.c"          /* 拿到 static 的 json_* / utf8_truncate / classify */

static int fails, total;

static void cki(const char *what, int got, int want)
{
    total++;
    if (got != want) { printf("  FAIL %-46s got=%d want=%d\n", what, got, want); fails++; }
    else             printf("  ok   %-46s = %d\n", what, got);
}

static void cks(const char *what, const char *got, const char *want)
{
    total++;
    if (!got || strcmp(got, want) != 0) {
        printf("  FAIL %-46s got=%s\n", what, got ? got : "(null)");
        fails++;
    } else {
        printf("  ok   %-46s = \"%s\"\n", what, got);
    }
}

static void ckhas(const char *what, const char *hay, const char *needle)
{
    total++;
    if (!hay || !strstr(hay, needle)) {
        printf("  FAIL %-46s 找不到「%s」（实际：%.60s）\n", what, needle, hay ? hay : "(null)");
        fails++;
    } else {
        printf("  ok   %-46s 含「%s」\n", what, needle);
    }
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  打不开 fixture：%s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t r = fread(buf, 1, (size_t)n, f);
    buf[r] = 0;
    fclose(f);
    return buf;
}

static const char *FIX(const char *name)
{
    static char path[256];
    snprintf(path, sizeof(path), "tools/testdata/%s", name);
    return path;
}

/* 让 classify() 看到一个「curl 正常退出 + 状态码 st + 响应体文件」的场景 */
static void run_case(const char *fixture, const char *http)
{
    snprintf(g_resp, sizeof(g_resp), "%s", FIX(fixture));
    FILE *f = fopen("/tmp/ai-parse-test.status", "wb");
    fputs(http, f);
    fclose(f);
    snprintf(g_status, sizeof(g_status), "%s", "/tmp/ai-parse-test.status");
    snprintf(g_errfile, sizeof(g_errfile), "%s", "/tmp/ai-parse-test.err");
    FILE *e = fopen("/tmp/ai-parse-test.err", "wb");
    fputs("", e);
    fclose(e);
    snprintf(g_url, sizeof(g_url), "%s", "http://169.254.134.123:3688/v1/chat/completions");
    g_reply[0] = 0;
    g_error[0] = 0;
    g_state = AI_IDLE;
    g_pending_ret = 0;
    classify(0);                     /* 参数是 waitpid 的 status：0 = 正常退出 */
}

int main(void)
{
    printf("AI 回复解析门禁（fixtures = 真实抓包）\n");

    printf("\n[1] 正常回复：message.content 要能找到，且不能抓到 reasoning_content\n");
    {
        char *body = slurp(FIX("ai_ok.json"));
        const char *choice = json_first_choice(body);
        cki("找到 choices[0]", choice != NULL, 1);
        const char *msg = choice ? json_message_of(choice) : NULL;
        cki("找到 message 对象", msg != NULL, 1);
        const char *vs = NULL;
        int vl = 0;
        cki("message.content 存在", obj_find_str(msg, "content", &vs, &vl), 1);
        char out[1024];
        json_unescape(vs, vl, out, sizeof(out));
        cks("content 解出来是原话", out, "我很好，随时能帮你处理石溪相机和GEC6818开发板的事。");
        const char *vs2 = NULL;
        int vl2 = 0;
        cki("reasoning_content 也在（这条是空串）",
            obj_find_str(msg, "reasoning_content", &vs2, &vl2), 1);
        cki("两者不是同一个位置", vs != vs2, 1);
        cki("reasoning_content 长度 0", vl2, 0);
        free(body);
    }

    printf("\n[2] 对抗用例：content 里带转义引号，reasoning 里也出现 \"content\" 这个词\n");
    {
        char *body = slurp(FIX("ai_quotes.json"));
        const char *choice = json_first_choice(body);
        const char *msg = json_message_of(choice);
        const char *vs = NULL, *vr = NULL;
        int vl = 0, vrlen = 0;
        cki("content 找得到", obj_find_str(msg, "content", &vs, &vl), 1);
        cki("reasoning_content 找得到（非空）", obj_find_str(msg, "reasoning_content", &vr, &vrlen), 1);
        cki("reasoning_content 确实非空", vrlen > 0, 1);
        char out[1024], rsn[1024];
        json_unescape(vs, vl, out, sizeof(out));
        json_unescape(vr, vrlen, rsn, sizeof(rsn));
        cks("content = 那段 JSON 原文", out, "{\"content\":\"x\",\"note\":\"带引号\"}");
        cki("两者内容不同（没把 reasoning 当 content）", strcmp(out, rsn) != 0, 1);
        cki("reasoning 里含 content 字样（说明真会撞车）", strstr(rsn, "content") != NULL, 1);
        free(body);
    }

    printf("\n[3] 截断：content 空 + finish_reason=length\n");
    {
        char *body = slurp(FIX("ai_length.json"));
        const char *choice = json_first_choice(body);
        const char *msg = json_message_of(choice);
        const char *vs = NULL;
        int vl = -1;
        cki("content 键存在但为空串", obj_find_str(msg, "content", &vs, &vl), 1);
        cki("空串长度 0", vl, 0);
        const char *fr = NULL;
        int frl = 0;
        cki("finish_reason 读得到", obj_find_str(choice, "finish_reason", &fr, &frl), 1);
        cki("finish_reason = length", frl == 6 && strncmp(fr, "length", 6) == 0, 1);
        free(body);
    }

    printf("\n[4] 两种错误体形状\n");
    {
        char *b401 = slurp(FIX("ai_401.json"));
        char *b400 = slurp(FIX("ai_400.json"));
        char out[256];
        cki("401 的 error 是字符串，取得到", json_error_text(b401, out, sizeof(out)), 1);
        cks("401 文案", out, "Invalid proxy access key");
        cki("400 的 error 是对象，取得到 message", json_error_text(b400, out, sizeof(out)), 1);
        ckhas("400 文案里列出了合法模型名", out, "supported API model names");
        free(b401);
        free(b400);
    }

    printf("\n[5] 反转义与 UTF-8 边界\n");
    {
        char out[128];
        const char *s1 = "a\\nb\\tc\\\"d\\\\e";
        json_unescape(s1, (int)strlen(s1), out, sizeof(out));
        cks("\\n \\t \\\" \\\\", out, "a\nb\tc\"d\\e");
        const char *s2 = "\\u4e2d\\u6587";
        json_unescape(s2, (int)strlen(s2), out, sizeof(out));
        cks("\\uXXXX → 中文", out, "中文");
        const char *s3 = "\\ud83d\\ude00";
        json_unescape(s3, (int)strlen(s3), out, sizeof(out));
        cks("代理对 → emoji", out, "\U0001F600");
        const char *s4 = "\\ud83d";
        json_unescape(s4, (int)strlen(s4), out, sizeof(out));
        cks("落单代理 → U+FFFD", out, "\uFFFD");
        const char *s5 = "\\uZZZZ";
        json_unescape(s5, (int)strlen(s5), out, sizeof(out));
        cks("坏 \\u 转义 → U+FFFD", out, "\uFFFD");

        char buf[16];
        snprintf(buf, sizeof(buf), "你好世界");            /* 12 字节 */
        utf8_truncate(buf, 7);                             /* 7 落在「世」中间 */
        cks("按 UTF-8 边界截断（不切半个字）", buf, "你好");
        snprintf(buf, sizeof(buf), "你好世界");
        utf8_truncate(buf, 12);
        cks("没超长就不动", buf, "你好世界");
    }

    printf("\n[6] 转义后写出去的文件能被自己解回来（往返一致）\n");
    {
        const char *src = "先\"后\\n换行\t制表 汉字😀";
        char *buf = NULL;
        size_t cap = 0;
        FILE *m = open_memstream(&buf, &cap);
        json_esc_append(m, src);
        fclose(m);
        ckhas("写出去的是转义形式", buf, "\\\"");
        char back[256];
        json_unescape(buf, (int)strlen(buf), back, sizeof(back));
        cks("往返一致", back, src);
        free(buf);
    }

    printf("\n[7] classify() 端到端：界面看到的成功/失败文案\n");
    {
        run_case("ai_ok.json", "200");
        cki("200 正常 → AI_OK", g_state, AI_OK);
        cks("回复文本", g_reply, "我很好，随时能帮你处理石溪相机和GEC6818开发板的事。");
        cki("历史里记了一轮", g_nhist, 1);

        run_case("ai_length.json", "200");
        cki("内容为空 → AI_ERR", g_state, AI_ERR);
        ckhas("提示是「截断」而不是「解析失败」", g_error, "截断");

        run_case("ai_401.json", "401");
        cki("401 → AI_ERR", g_state, AI_ERR);
        ckhas("提示检查密钥", g_error, "密钥被拒");

        run_case("ai_400.json", "400");
        cki("400 → AI_ERR", g_state, AI_ERR);
        ckhas("把服务端 message 原样带出来", g_error, "supported API model names");

        run_case("ai_ok.json", "500");
        cki("5xx → AI_ERR", g_state, AI_ERR);
        ckhas("提示服务端错误", g_error, "服务端错误 500");
    }

    printf("\n[8] curl 退出码 → 文案（连不上 / 超时）\n");
    {
        snprintf(g_url, sizeof(g_url), "%s", "http://169.254.134.123:3688/v1/chat/completions");
        g_error[0] = 0;
        g_state = AI_IDLE;
        g_pending_ret = 0;
        classify(7 << 8);
        cki("退出码 7 → AI_ERR", g_state, AI_ERR);
        ckhas("文案里有目标地址", g_error, "169.254.134.123:3688");
        ckhas("提示连不上", g_error, "连不上宿主机");

        g_error[0] = 0;
        g_state = AI_IDLE;
        classify(28 << 8);
        ckhas("退出码 28 → 超时文案", g_error, "超时");
    }

    unlink("/tmp/ai-parse-test.status");
    unlink("/tmp/ai-parse-test.err");
    printf("\n%s（断言 %d 项，失败 %d 项）\n", fails ? "有失败" : "解析门禁通过", total, fails);
    return fails ? 1 : 0;
}
