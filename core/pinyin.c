/*
 * pinyin.c — 拼音查表实现（表在 core/pinyin_dict.h，按音节升序，二分查找）
 */
#include "pinyin.h"

#include <string.h>

#include "pinyin_dict.h"

#define MAX_SYLLABLE 6      /* zhuang / chuang / shuang */

const char *pinyin_lookup(const char *py)
{
    if (!py || !*py) return NULL;
    int lo = 0, hi = PY_DICT_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int c = strcmp(py, PY_DICT[mid].py);
        if (c == 0) return PY_DICT[mid].hz;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

/* 是否存在以 py 开头的音节（即「还能继续打字」） */
static int has_prefix(const char *py)
{
    size_t n = strlen(py);
    if (n == 0) return 0;
    int lo = 0, hi = PY_DICT_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int c = strncmp(PY_DICT[mid].py, py, n);
        if (c < 0) lo = mid + 1;
        else if (c > 0) hi = mid - 1;
        else return 1;      /* 该项前 n 个字节就是 py */
    }
    return 0;
}

int pinyin_first_syllable(const char *py)
{
    size_t n = strlen(py);
    if (n == 0) return 0;
    if (n > MAX_SYLLABLE) n = MAX_SYLLABLE;
    for (int len = (int)n; len >= 1; len--) {
        char buf[MAX_SYLLABLE + 1];
        memcpy(buf, py, (size_t)len);
        buf[len] = 0;
        if (pinyin_lookup(buf)) return len;
    }
    return 0;
}

int pinyin_acceptable(const char *py)
{
    const char *p = py;
    if (!p || !*p) return 0;
    while (*p) {
        int len = pinyin_first_syllable(p);
        if (!len) break;
        p += len;
    }
    return (*p == 0) || has_prefix(p);
}
