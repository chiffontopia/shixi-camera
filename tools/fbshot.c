/* fbshot - capture the VISIBLE framebuffer region to a raw file (for dev verification) */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "/tmp/fb.raw";
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("open fb0"); return 1; }
    struct fb_var_screeninfo vi;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi)) { perror("FBIOGET_VSCREENINFO"); return 1; }
    int W = vi.xres, H = vi.yres, VW = vi.xres_virtual, VH = vi.yres_virtual;
    size_t mapsize = (size_t)VW * VH * (vi.bits_per_pixel / 8);
    unsigned char *fb = mmap(NULL, mapsize, PROT_READ, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }
    size_t bpp = vi.bits_per_pixel / 8;
    unsigned char *src = fb + (size_t)vi.yoffset * VW * bpp + (size_t)vi.xoffset * bpp;
    FILE *f = fopen(out, "wb");
    if (!f) { perror("fopen"); return 1; }
    /* write a small text header then raw pixels */
    fprintf(f, "FB %d %d %d %d %d %d\n", W, H, vi.bits_per_pixel, vi.red.offset, vi.green.offset, vi.blue.offset);
    for (int y = 0; y < H; y++) fwrite(src + (size_t)y * VW * bpp, 1, (size_t)W * bpp, f);
    fclose(f);
    munmap(fb, mapsize); close(fd);
    printf("saved %s (visible %dx%d, yoffset=%d)\n", out, W, H, vi.yoffset);
    return 0;
}
