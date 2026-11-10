/* raster.c — 同一份工作負載的 C 版（-O2），用來對照 Python / JS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 1232
#define H 1488
#define LINES 44
#define COLS 72

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void do_clear(unsigned char *b) {
    memset(b, 0x0B, (size_t)W * H * 3);
}

static void do_solid(unsigned char *b) {
    for (int ln = 0; ln < LINES; ln++) {
        int y = 20 + ln * 16;
        for (int c = 0; c < COLS; c++) {
            int x = 20 + c * 8;
            for (int dy = 0; dy < 12; dy++)
                memset(b + ((size_t)(y + dy) * W + x) * 3, 0xE8, 7 * 3);
        }
    }
}

static void do_ink(unsigned char *b) {
    unsigned seed = 12345u;
    for (int ln = 0; ln < LINES; ln++) {
        int y = 20 + ln * 16;
        for (int c = 0; c < COLS; c++) {
            int x = 20 + c * 8;
            for (int dy = 0; dy < 12; dy++) {
                seed = seed * 1103515245u + 12345u;
                if (((seed >> 16) % 100) < 22) {
                    int x0 = (seed >> 5) % 6;
                    memset(b + ((size_t)(y + dy) * W + x + x0) * 3, 0xE8, 3 * 3);
                }
            }
        }
    }
}

int main(void) {
    unsigned char *b = malloc((size_t)W * H * 3);
    if (!b) return 1;
    double best[3] = {1e9, 1e9, 1e9};
    void (*fn[3])(unsigned char *) = {do_clear, do_solid, do_ink};
    const char *name[3] = {"clear", "solid", "ink"};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 20; i++) {
            double t = now();
            fn[k](b);
            double dt = now() - t;
            if (dt < best[k]) best[k] = dt;
        }
    printf("C(-O2)  %dx%d\n", W, H);
    for (int k = 0; k < 3; k++) printf("  %-6s %7.2f ms\n", name[k], best[k]);
    free(b);
    return 0;
}
