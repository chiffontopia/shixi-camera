/*
 * util.h — 小工具：时间、数学、字符串
 */
#ifndef SHIXI_UTIL_H
#define SHIXI_UTIL_H

#include <stdint.h>
#include <time.h>
#include <sys/time.h>

static inline uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static inline uint64_t wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float fclampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* 缓动：0..1 -> 0..1 */
static inline float ease_out_cubic(float t)
{
    float u = 1.0f - fclampf(t, 0.0f, 1.0f);
    return 1.0f - u * u * u;
}

static inline float ease_in_out(float t)
{
    t = fclampf(t, 0.0f, 1.0f);
    return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
}

/* 把毫秒格式化成 mm:ss */
static inline void fmt_duration(int sec, char *buf, int buflen)
{
    if (sec < 0) sec = 0;
    int m = sec / 60, s = sec % 60;
    if (m > 99) { m = 99; s = 59; }
    /* 简易 snprintf，避免额外依赖 */
    buf[0] = (char)('0' + m / 10);
    buf[1] = (char)('0' + m % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + s / 10);
    buf[4] = (char)('0' + s % 10);
    buf[5] = 0;
    (void)buflen;
}

#endif /* SHIXI_UTIL_H */
