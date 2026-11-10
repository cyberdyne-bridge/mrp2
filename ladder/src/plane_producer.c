/* plane_producer — 模擬解碼器：以固定 Hz 更新共享記憶體影片緩衝
 * 只改「舊豎條 ∪ 新豎條」的列，模擬真實解碼器只寫變化區域
 * 用法：plane_producer <file> [hz]
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define W 1920
#define H 1080
#define BW 120          /* 豎條寬 */

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void bar(uint32_t *p, int x0, int x1, uint32_t c) {
    if (x0 < 0) x0 = 0;
    if (x1 > W) x1 = W;
    if (x1 <= x0) return;
    int y0 = H / 3, y1 = H * 2 / 3;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) p[(size_t)y * W + x] = c;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "用法：plane_producer <file> [hz]\n"); return 2; }
    int hz = argc > 2 ? atoi(argv[2]) : 60;
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    size_t bytes = (size_t)W * H * 4;
    uint32_t *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }

    /* 背景：上到下漸層 */
    for (int y = 0; y < H; y++) {
        uint32_t c = 0x00000000u | ((20 + 200 * y / H) << 16) |
                     ((30 + 180 * y / H) << 8) | 120;
        for (int x = 0; x < W; x++) p[(size_t)y * W + x] = c;
    }

    printf("producer: %dx%d @ %d Hz，共享記憶體 %.1f MB\n",
           W, H, hz, bytes / 1048576.0);
    fflush(stdout);

    double next = now_ms();
    int last = 0;
    for (int f = 1;; f++) {
        int nb = (f * 12) % (W + 200) - 100;
        /* 只抹掉舊豎條、畫新豎條（模擬解碼器寫變化區域） */
        bar(p, last, last + BW, 0x00000000u | ((20 + 200 * (H/2) / H) << 16) |
                                  ((30 + 180 * (H/2) / H) << 8) | 120);
        bar(p, nb, nb + BW, 0x00F0A020u);
        last = nb;

        next += 1000.0 / hz;
        double sleep = next - now_ms();
        if (sleep > 0) {
            struct timespec ts = {(time_t)(sleep / 1000),
                                  (long)((sleep - (long)(sleep / 1000) * 1000) * 1e6)};
            nanosleep(&ts, NULL);
        } else {
            next = now_ms();
        }
    }
    return 0;
}
