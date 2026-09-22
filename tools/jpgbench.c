/* jpgbench.c — 测板子上 JPEG 编码速度（录像/拍照的性能瓶颈就在这里） */
#include "gfx.h"
#include "image.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 640;
    int h = argc > 2 ? atoi(argv[2]) : 480;
    int n = argc > 3 ? atoi(argv[3]) : 10;
    int q = argc > 4 ? atoi(argv[4]) : 82;
    Surface *s = gfx_surface_new(w, h);
    if (!s) return 1;
    /* 造一张有细节的图，避免纯色让编码器跑得太乐观 */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            s->px[(size_t)y * s->stride + x] =
                RGB((x * 7 + y * 3) & 0xFF, (x * x / 7 + y) & 0xFF, (x + y * 5) & 0xFF);
    uint64_t t0 = now_ms();
    int total = 0;
    for (int i = 0; i < n; i++) {
        unsigned char *buf = NULL;
        int len = 0;
        if (image_encode_jpeg_mem(s, q, &buf, &len) == 0) { total += len; free(buf); }
    }
    uint64_t dt = now_ms() - t0;
    printf("%dx%d 质量%d：编码 %d 次共 %llu ms，平均 %.1f ms/帧 => 上限 %.1f fps（平均 %d 字节）\n",
           w, h, q, n, (unsigned long long)dt, (double)dt / n, n * 1000.0 / (dt ? dt : 1), total / (n ? n : 1));

    /* 顺便测缩放贴图（预览）的耗时 */
    Surface *dst = gfx_surface_new(800, 480);
    t0 = now_ms();
    for (int i = 0; i < n; i++) gfx_blit_cover(dst, 0, 0, 800, 480, s, 255);
    dt = now_ms() - t0;
    printf("预览缩放 %dx%d->800x480：%.1f ms/帧 => 上限 %.1f fps\n", w, h,
           (double)dt / n, n * 1000.0 / (dt ? dt : 1));
    return 0;
}
