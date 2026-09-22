/*
 * ai.c — 板端 LLM 客户端（OpenAI 兼容 /v1/chat/completions）
 *
 * 全部决定与理由见 .scratch/board-desktop/design/11-ai-assistant-transport.md。
 * 要点回顾：
 *   · 子进程 = 静态 curl（板上是 $SHIXI_ROOT/curl，主机模拟器用 PATH 里的 curl）
 *   · 请求体写文件后用 --data-binary @file，命令行里不出现 JSON（转义地狱 + ARG_MAX）
 *   · 密钥写进 curl 的 -K 配置文件（0600），argv 里没有密钥 → ps 看不到
 *   · exec 前 close(3..)：别把 /dev/fb0、触摸设备和 flock 单实例锁带给子进程
 *     （工单 21 实测 mplayer 会继承它们；curl 短命，但卡住时同样会攥着锁）
 *   · 只保留最近 N 轮；**失败/取消的轮次不写历史**，所以重试不会重复记录
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "ai.h"
#include "media.h"
#include "util.h"

#define AI_MAX_ROUNDS  8
#define AI_REPLY_MAX   1024          /* 与 app_chat.c 的 ChatMsg.text 一致 */
#define AI_ERR_MAX     256
#define AI_USER_MAX    512
#define AI_CONF_MAX    1024

/* 系统提示词：屏幕只有 800×480，且模型是推理模型（思维链不计入这里），
 * 明确要求短答，否则一口气二十行。 */
#define AI_SYS_PROMPT  "你是石溪相机（GEC6818 开发板）里的助手小石。回答要简短，最多三句话，" \
                       "直接给结论，不要用 Markdown 标记。"

typedef struct {
    char user[AI_USER_MAX];
    char asst[AI_REPLY_MAX];
} AiRound;

static char g_url[256], g_key[256], g_model[64], g_curl[512];
static char g_req[512], g_resp[512], g_status[512], g_errfile[512], g_curlcfg[512];
static char g_conf[512];
static int  g_max_tokens = 800, g_timeout = 20, g_history_n = 3;
static int  g_ready = 0;

static AiState  g_state = AI_IDLE;
static pid_t    g_pid = -1;
static uint64_t g_deadline = 0;
static int      g_pending_ret = 0;         /* 结果还没交给调用方 */
static char     g_pending_user[AI_USER_MAX];
static uint64_t g_t0 = 0;                  /* 本轮开始时间（日志用） */
static int      g_reqno = 0;
static char     g_reply[AI_REPLY_MAX];
static char     g_error[AI_ERR_MAX];

static AiRound g_hist[AI_MAX_ROUNDS];
static int     g_nhist = 0;                /* 已完成的轮数 */
static int     g_head  = 0;                /* 下一个写入位置 */

/* ---------------- 小工具 ---------------- */

/* 一行 stderr 日志：板上没有别的调试手段，这行就是「这句到底谁答的」的证据 */
static void ai_log(int ok, const char *what)
{
    double sec = g_t0 ? (double)(now_ms() - g_t0) / 1000.0 : 0.0;
    if (ok) fprintf(stderr, "ai: 第 %d 轮 %s 回复 %zu 字（%.1fs）\n",
                    g_reqno, g_model, strlen(g_reply), sec);
    else    fprintf(stderr, "ai: 第 %d 轮失败（%.1fs）：%s\n", g_reqno, sec, what);
}

static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
    ai_log(0, g_error);
    g_state = AI_ERR;
    g_pending_ret = 1;
}

/* 读小文本文件并去掉尾部空白；返回 1 = 读到非空内容 */
static int read_trim(const char *path, char *out, int n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t r = fread(out, 1, (size_t)n - 1, f);
    fclose(f);
    out[r] = 0;
    /* 允许文件带 UTF-8 BOM（在 Windows 上编辑过） */
    if (r >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF) {
        memmove(out, out + 3, r - 3 + 1);
        r -= 3;
    }
    while (r > 0) {
        char c = out[r - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') out[--r] = 0;
        else break;
    }
    return out[0] ? 1 : 0;
}

/* ai.conf 里取一个 key=value（只认第一个匹配；行内 # 之后算注释） */
static int conf_get(const char *key, char *out, int n)
{
    char buf[AI_CONF_MAX];
    if (!read_trim(g_conf, buf, sizeof(buf))) return 0;
    int klen = (int)strlen(key);
    char *p = buf;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *s = p;
        while (*s == ' ' || *s == '\t') s++;
        if (strncmp(s, key, (size_t)klen) == 0) {
            char *q = s + klen;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                char *e = q;
                while (*e && *e != '#' && *e != '\r' && *e != ' ' && *e != '\t') e++;
                *e = 0;
                if (*q) { snprintf(out, (size_t)n, "%s", q); return 1; }
                return 0;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    return 0;
}

static int conf_get_int(const char *key, int *out)
{
    char v[32];
    if (!conf_get(key, v, sizeof(v))) return 0;
    int n = atoi(v);
    if (n <= 0) return 0;
    *out = n;
    return 1;
}

/* 从 URL 里取出 host:port 给错误文案用（连不上时能直接看到打的是哪个地址） */
static void url_host(const char *url, char *out, int n)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    int i = 0;
    while (p[i] && p[i] != '/' && i < n - 1) { out[i] = p[i]; i++; }
    out[i] = 0;
    if (!out[0]) snprintf(out, (size_t)n, "宿主机");
}

/* ---------------- JSON ---------------- */

/* 转义后直接写进文件（UTF-8 原样透传，控制字符走 \u00XX） */
static void json_esc_append(FILE *f, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        case '\b': fputs("\\b", f);  break;
        case '\f': fputs("\\f", f);  break;
        default:
            if (c < 0x20) fprintf(f, "\\u%04x", c);
            else fputc(c, f);
        }
    }
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = v * 16 + d;
    }
    return v;
}

static int utf8_put(uint32_t cp, char *out, int avail)
{
    if (cp < 0x80) { if (avail < 1) return 0; out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        if (avail < 2) return 0;
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        if (avail < 3) return 0;
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (avail < 4) return 0;
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* JSON 字符串值（不含两端引号，长度 len）→ 反转义后落进 out */
static void json_unescape(const char *s, int len, char *out, int n)
{
    int oi = 0;
    for (int i = 0; i < len && oi < n - 1; i++) {
        if (s[i] != '\\') { out[oi++] = s[i]; continue; }
        if (++i >= len) break;
        switch (s[i]) {
        case 'n': out[oi++] = '\n'; break;
        case 't': out[oi++] = '\t'; break;
        case 'r': out[oi++] = '\r'; break;
        case 'b': out[oi++] = '\b'; break;
        case 'f': out[oi++] = '\f'; break;
        case '"': out[oi++] = '"';  break;
        case '\\': out[oi++] = '\\'; break;
        case '/': out[oi++] = '/';  break;
        case 'u': {
            if (i + 4 >= len) { i = len; break; }
            int cp = hex4(s + i + 1);
            i += 4;
            if (cp < 0) cp = 0xFFFD;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < len &&
                s[i + 1] == '\\' && s[i + 2] == 'u') {
                int lo = hex4(s + i + 3);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
            oi += utf8_put((uint32_t)cp, out + oi, n - 1 - oi);
            break;
        }
        default: out[oi++] = s[i]; break;
        }
    }
    out[oi] = 0;
}

/* 跳过一段 JSON 值（跨得过字符串/对象/数组），返回其后的位置 */
static const char *json_skip(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '"') {
        p++;
        while (*p) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == '"') { p++; break; }
            p++;
        }
        return p;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = json_skip(p); continue; }
            if (*p == open) depth++;
            else if (*p == close) { depth--; p++; if (depth == 0) break; continue; }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* 在对象 obj（指向 '{'）的第一层找 key 的**字符串**值。
 * 注意：必须按 key 精确匹配，"content" 才不会命中 "reasoning_content"（推理模型会带它）。 */
static int obj_find_str(const char *obj, const char *key, const char **vs, int *vl)
{
    if (!obj || *obj != '{') return 0;
    const char *p = obj + 1;
    int klen = (int)strlen(key);
    for (;;) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        if (*p != '"') return 0;
        const char *ks = ++p;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        int len = (int)(p - ks);
        if (*p == '"') p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != ':') return 0;
        p++;                                  /* ← 别忘了跨过冒号（第一次就漏了，见 tools/ai_parse_test.c） */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (len == klen && strncmp(ks, key, (size_t)klen) == 0) {
            if (*p != '"') return 0;               /* null / 对象 → 当作没有 */
            const char *v0 = ++p;
            while (*p) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                if (*p == '"') break;
                p++;
            }
            *vs = v0;
            *vl = (int)(p - v0);
            return 1;
        }
        p = json_skip(p);
        if (*p != ',') return 0;
    }
}

static const char *json_first_choice(const char *json)
{
    const char *p = strstr(json, "\"choices\"");
    if (!p) return NULL;
    p = strchr(p, '[');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return (*p == '{') ? p : NULL;
}

static const char *json_message_of(const char *choice)
{
    const char *k = strstr(choice, "\"message\"");
    if (!k) return NULL;
    const char *b = strchr(k, '{');
    return b;
}

/* 错误体：{"error":"字符串"}（401）与 {"error":{"message":"…"}}（400）两种形状都吃 */
static int json_error_text(const char *json, char *out, int n)
{
    const char *p = strstr(json, "\"error\"");
    if (!p) return 0;
    p += 7;
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p == '"') {
        const char *v0 = ++p;
        while (*p) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == '"') break;
            p++;
        }
        json_unescape(v0, (int)(p - v0), out, n);
        return out[0] ? 1 : 0;
    }
    if (*p == '{') {
        const char *vs;
        int vl;
        if (obj_find_str(p, "message", &vs, &vl)) {
            json_unescape(vs, vl, out, n);
            return out[0] ? 1 : 0;
        }
    }
    return 0;
}

/* 按 UTF-8 边界截断（绝不从半个汉字中间切） */
static void utf8_truncate(char *buf, int max)
{
    int len = (int)strlen(buf);
    if (len <= max) return;
    int i = max;
    while (i > 0 && ((unsigned char)buf[i] & 0xC0) == 0x80) i--;
    buf[i] = 0;
}

/* ---------------- 配置 ---------------- */

int ai_init(void)
{
    const char *tmp = media_tmp_dir();
    const char *root = media_root();
    if (!tmp || !*tmp) tmp = "/tmp";
    if (!root || !*root) root = "/tmp";

    snprintf(g_req,      sizeof(g_req),      "%s/ai_req.json", tmp);
    snprintf(g_resp,     sizeof(g_resp),     "%s/ai_resp.json", tmp);
    snprintf(g_status,   sizeof(g_status),   "%s/ai_status.txt", tmp);
    snprintf(g_errfile,  sizeof(g_errfile),  "%s/ai_err.txt", tmp);
    snprintf(g_curlcfg,  sizeof(g_curlcfg),  "%s/ai_curl.cfg", tmp);
    snprintf(g_conf,     sizeof(g_conf),     "%s/ai.conf", root);

    char p[512], v[64];
    const char *e;

    g_url[0] = g_key[0] = g_model[0] = 0;
    g_max_tokens = 800;
    g_timeout = 20;
    g_history_n = 3;

    if ((e = getenv("SHIXI_LLM_URL")) && *e) snprintf(g_url, sizeof(g_url), "%s", e);
    else { snprintf(p, sizeof(p), "%s/relay_url", root); read_trim(p, g_url, sizeof(g_url)); }

    if ((e = getenv("SHIXI_LLM_KEY")) && *e) snprintf(g_key, sizeof(g_key), "%s", e);
    else { snprintf(p, sizeof(p), "%s/relay_key", root); read_trim(p, g_key, sizeof(g_key)); }

    if ((e = getenv("SHIXI_LLM_MODEL")) && *e) snprintf(g_model, sizeof(g_model), "%s", e);
    else conf_get("model", g_model, sizeof(g_model));
    if (!g_model[0]) snprintf(g_model, sizeof(g_model), "deepseek-flash");

    int n;
    if (conf_get_int("max_tokens", &n)) g_max_tokens = n;
    if (conf_get_int("timeout", &n)) g_timeout = n;
    if (conf_get_int("history", &n) && n <= AI_MAX_ROUNDS) g_history_n = n;

    if ((e = getenv("SHIXI_CURL")) && *e) snprintf(g_curl, sizeof(g_curl), "%s", e);
    else {
        snprintf(p, sizeof(p), "%s/curl", root);
        if (access(p, X_OK) == 0) snprintf(g_curl, sizeof(g_curl), "%s", p);
        else snprintf(g_curl, sizeof(g_curl), "curl");
    }
    (void)v;
    g_ready = 1;
    return g_url[0] ? 1 : 0;
}

int ai_configured(void) { return g_url[0] ? 1 : 0; }
const char *ai_model(void) { return g_model; }
const char *ai_reply(void) { return g_reply; }
const char *ai_error(void) { return g_error; }
const char *ai_last_user(void) { return g_pending_user; }

void ai_history_clear(void)
{
    g_nhist = 0;
    g_head = 0;
}

/* ---------------- 请求 ---------------- */

/* 密钥写进 curl 的 -K 配置文件：这样 argv 里只有文件名，ps / 日志都看不到密钥 */
static int write_curl_cfg(void)
{
    FILE *f = fopen(g_curlcfg, "wb");
    if (!f) return -1;
    chmod(g_curlcfg, 0600);
    fputs("header = \"Content-Type: application/json\"\n", f);
    fprintf(f, "header = \"Authorization: Bearer %s\"\n", g_key);
    if (fclose(f) != 0) return -1;
    return 0;
}

static int write_request(void)
{
    FILE *f = fopen(g_req, "wb");
    if (!f) return -1;
    fputs("{\"model\":\"", f);
    json_esc_append(f, g_model);
    fputs("\",\"messages\":[{\"role\":\"system\",\"content\":\"", f);
    json_esc_append(f, AI_SYS_PROMPT);
    fputs("\"},", f);

    int n = g_nhist < g_history_n ? g_nhist : g_history_n;
    for (int k = 0; k < n; k++) {
        int idx = (g_head - n + k + AI_MAX_ROUNDS * 2) % AI_MAX_ROUNDS;
        fputs("{\"role\":\"user\",\"content\":\"", f);
        json_esc_append(f, g_hist[idx].user);
        fputs("\"},{\"role\":\"assistant\",\"content\":\"", f);
        json_esc_append(f, g_hist[idx].asst);
        fputs("\"},", f);
    }
    fputs("{\"role\":\"user\",\"content\":\"", f);
    json_esc_append(f, g_pending_user);
    fprintf(f, "\"}],\"max_tokens\":%d,\"stream\":false}", g_max_tokens);
    if (fclose(f) != 0) return -1;
    return 0;
}

static int spawn_curl(void)
{
    char tmo[16];
    char reqarg[600];
    snprintf(tmo, sizeof(tmo), "%d", g_timeout);
    snprintf(reqarg, sizeof(reqarg), "@%s", g_req);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int fo = open(g_status, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int fe = open(g_errfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fo >= 0) { dup2(fo, 1); if (fo > 2) close(fo); }
        if (fe >= 0) { dup2(fe, 2); if (fe > 2) close(fe); }
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 1024) maxfd = 1024;
        for (int fd = 3; fd < (int)maxfd; fd++) close(fd);
        execlp(g_curl, "curl", "-sS", "--max-time", tmo, "--connect-timeout", "4",
               "-K", g_curlcfg, "--data-binary", reqarg,
               "-o", g_resp, "-w", "%{http_code}", g_url, (char *)NULL);
        _exit(127);
    }
    g_pid = pid;
    return 0;
}

void ai_ask(const char *text)
{
    if (g_state == AI_BUSY) return;
    if (!g_ready) ai_init();
    snprintf(g_pending_user, sizeof(g_pending_user), "%s", text ? text : "");
    g_reply[0] = 0;
    g_error[0] = 0;

    if (!g_url[0]) { set_err("没有配置模型地址（试试把 endpoint 写进 relay_url）"); return; }
    if (!g_key[0]) { set_err("缺少 relay_key（把宿主机 CCX 的密钥放到 %s/relay_key）",
                             media_root()); return; }
    /* 密钥会写进 curl 的配置文件，含引号/换行会把配置写坏 */
    if (strchr(g_key, '"') || strchr(g_key, '\n') || strchr(g_key, '\r')) {
        set_err("relay_key 里有非法字符（引号或换行）");
        return;
    }
    if (write_curl_cfg() != 0) { set_err("无法写临时配置 %s", g_curlcfg); return; }
    if (write_request() != 0)  { set_err("无法写请求文件 %s", g_req); return; }
    if (spawn_curl() != 0)     { set_err("启动 curl 失败（%s）", g_curl); return; }

    g_reqno++;
    g_t0 = now_ms();
    g_deadline = g_t0 + (uint64_t)(g_timeout + 5) * 1000;
    g_state = AI_BUSY;
}

void ai_cancel(void)
{
    if (g_pid > 0) {
        kill(g_pid, SIGTERM);
        int st = 0;
        if (waitpid(g_pid, &st, WNOHANG) == 0) {
            usleep(50000);
            if (waitpid(g_pid, &st, WNOHANG) == 0) {
                kill(g_pid, SIGKILL);
                waitpid(g_pid, &st, 0);
            }
        }
        g_pid = -1;
    }
    if (g_state == AI_BUSY) g_state = AI_IDLE;
    g_pending_ret = 0;
}

void ai_deinit(void)
{
    ai_cancel();
    unlink(g_req);
    unlink(g_resp);
    unlink(g_status);
    unlink(g_errfile);
    unlink(g_curlcfg);      /* 里面有密钥，别留在 /tmp */
    g_state = AI_IDLE;
    g_pending_ret = 0;
    g_ready = 0;            /* 下次 ai_ask 会重新读配置 */
}

/* ---------------- 结果判定 ---------------- */

static void commit_round(void)
{
    snprintf(g_hist[g_head].user, sizeof(g_hist[g_head].user), "%s", g_pending_user);
    snprintf(g_hist[g_head].asst, sizeof(g_hist[g_head].asst), "%s", g_reply);
    g_head = (g_head + 1) % AI_MAX_ROUNDS;
    if (g_nhist < AI_MAX_ROUNDS) g_nhist++;
}

static void read_first_line(const char *path, char *out, int n)
{
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t r = fread(out, 1, (size_t)n - 1, f);
    fclose(f);
    out[r] = 0;
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
}

static void classify(int status)
{
    if (WIFSIGNALED(status)) { set_err("请求被中断"); return; }

    int code = WEXITSTATUS(status);
    if (code != 0) {
        char host[128];
        url_host(g_url, host, sizeof(host));
        if (code == 6 || code == 7) {
            set_err("连不上宿主机（%s），确认 CCX 还开着", host);
        } else if (code == 28) {
            set_err("请求超时（%d 秒），稍后再试", g_timeout);
        } else {
            char line[160];
            read_first_line(g_errfile, line, sizeof(line));
            if (line[0]) set_err("curl 出错（%d）：%s", code, line);
            else         set_err("curl 出错，退出码 %d", code);
        }
        return;
    }

    char stbuf[32];
    read_first_line(g_status, stbuf, sizeof(stbuf));
    int http = atoi(stbuf);

    /* 读响应体 */
    static char body[8192];
    int blen = 0;
    FILE *f = fopen(g_resp, "rb");
    if (f) {
        size_t r = fread(body, 1, sizeof(body) - 1, f);
        fclose(f);
        body[r] = 0;
        blen = (int)r;
    } else {
        body[0] = 0;
    }
    if (blen == 0 && http == 0) { set_err("没有拿到任何响应（curl 没输出）"); return; }

    if (http >= 200 && http < 300) {
        const char *choice = json_first_choice(body);
        const char *msg = choice ? json_message_of(choice) : NULL;
        const char *vs = NULL;
        int vl = 0;
        if (msg && obj_find_str(msg, "content", &vs, &vl)) {
            json_unescape(vs, vl, g_reply, sizeof(g_reply));
        } else if (!msg) {
            /* 可能是 {"error":…} 形式的 200（后端自造的错） */
            char emsg[AI_ERR_MAX];
            if (json_error_text(body, emsg, sizeof(emsg))) set_err("后端报错：%s", emsg);
            else set_err("回复解析失败（没有 message）");
            return;
        }
        if (!g_reply[0]) {
            const char *fr = NULL;
            int frl = 0;
            if (choice && obj_find_str(choice, "finish_reason", &fr, &frl) &&
                frl == 6 && strncmp(fr, "length", 6) == 0) {
                set_err("回复被 max_tokens 截断了（模型先把额度用在思考上），再说一次更短的问题");
            } else {
                set_err("模型没有给出内容（可能是空回复，重试一次）");
            }
            return;
        }
        /* 超长按 UTF-8 边界截断（绝不从半个汉字中间切），并说明是截断过的 */
        if (strlen(g_reply) > AI_REPLY_MAX - 40) {
            utf8_truncate(g_reply, AI_REPLY_MAX - 40);
            strcat(g_reply, "…（太长，已截断）");
        }
        commit_round();
        ai_log(1, NULL);
        g_state = AI_OK;
        g_pending_ret = 1;
        return;
    }

    if (http == 401 || http == 403) {
        set_err("密钥被拒（检查 %s/relay_key）", media_root());
        return;
    }
    if (http == 400) {
        char emsg[AI_ERR_MAX];
        if (json_error_text(body, emsg, sizeof(emsg))) set_err("模型拒绝了请求：%s", emsg);
        else set_err("模型拒绝了请求（HTTP 400）");
        return;
    }
    if (http >= 500) { set_err("服务端错误 %d", http); return; }
    set_err("HTTP %d", http);
}

AiState ai_poll(void)
{
    if (g_state != AI_BUSY) {
        if (g_pending_ret) {
            AiState s = g_state;
            g_state = AI_IDLE;
            g_pending_ret = 0;
            return s;
        }
        return AI_IDLE;
    }

    int status = 0;
    pid_t r = waitpid(g_pid, &status, WNOHANG);
    if (r == 0) {
        if (g_deadline && now_ms() > g_deadline) {
            kill(g_pid, SIGKILL);
            waitpid(g_pid, &status, 0);
            g_pid = -1;
            set_err("请求超时（%d 秒），稍后再试", g_timeout);
        }
        return AI_BUSY;
    }
    if (r > 0) {
        classify(status);
        g_pid = -1;
    } else {
        /* ECHILD：子进程已经收过了 */
        set_err("请求异常结束");
    }
    return AI_BUSY;      /* 结果下一帧交出去（每帧只报一次状态） */
}
