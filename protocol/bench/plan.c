/* plan.c — 合成決策核心的 C 版（對照 Python compositor.plan）
 * 同一種場景：N 個 surface、每幀 M 個 damage 矩形
 * 編譯：gcc -O2 -o /var/tmp/plan plan.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_S 256
#define MAX_D 128
#define MAX_P 256

typedef struct { int x, y, w, h; } Rect;
typedef struct { int x, y, w, h, z, layer, opaque, visible; } Surf;

static Rect rect(int x, int y, int w, int h) { Rect r = {x, y, w, h}; return r; }
static int area(Rect r) { return r.w > 0 && r.h > 0 ? r.w * r.h : 0; }

static int inter(Rect a, Rect b, Rect *out) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w < b.x + b.w) ? a.x + a.w : b.x + b.w;
    int y1 = (a.y + a.h < b.y + b.h) ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0) return 0;
    *out = rect(x0, y0, x1 - x0, y1 - y0);
    return 1;
}

static int subtract(Rect r, Rect cut, Rect *out) {
    Rect i;
    if (!inter(r, cut, &i)) { out[0] = r; return 1; }
    int n = 0;
    if (i.y > r.y) out[n++] = rect(r.x, r.y, r.w, i.y - r.y);
    if (i.y + i.h < r.y + r.h) out[n++] = rect(r.x, i.y + i.h, r.w, r.y + r.h - (i.y + i.h));
    if (i.x > r.x) out[n++] = rect(r.x, i.y, i.x - r.x, i.h);
    if (i.x + i.w < r.x + r.w) out[n++] = rect(i.x + i.w, i.y, r.x + r.w - (i.x + i.w), i.h);
    return n;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static Surf S[MAX_S];
static int ORDER[MAX_S];
static int NS = 0;

static int cmp_rank(const void *a, const void *b) {
    int i = *(const int *)a, j = *(const int *)b;
    if (S[i].layer != S[j].layer) return S[i].layer - S[j].layer;
    if (S[i].z != S[j].z) return S[i].z - S[j].z;
    return i - j;
}

/* 回傳這個 damage 矩形內實際要合成的像素數 */
static long collect(Rect d) {
    Rect pieces[MAX_P];
    int np = 1;
    pieces[0] = d;
    long px = 0;
    for (int k = NS - 1; k >= 0; k--) {
        int si = ORDER[k];
        if (!S[si].visible) continue;
        Rect sr = rect(S[si].x, S[si].y, S[si].w, S[si].h);
        Rect nxt[MAX_P];
        int nn = 0;
        for (int p = 0; p < np; p++) {
            Rect it;
            if (!inter(pieces[p], sr, &it)) { nxt[nn++] = pieces[p]; continue; }
            px += area(it);
            if (S[si].opaque) {
                Rect parts[4];
                int m = subtract(pieces[p], it, parts);
                for (int q = 0; q < m && nn < MAX_P; q++) nxt[nn++] = parts[q];
            } else {
                nxt[nn++] = pieces[p];
            }
        }
        memcpy(pieces, nxt, sizeof(Rect) * nn);
        np = nn;
        if (!np) break;
    }
    return px;
}

int main(void) {
    /* 200 個視窗，隨機位置與大小；每幀 50 個 damage 矩形 */
    unsigned seed = 20261110u;
    NS = 200;
    for (int i = 0; i < NS; i++) {
        seed = seed * 1103515245u + 12345u;
        S[i].x = (seed >> 8) % 1800;
        S[i].y = (seed >> 4) % 1000;
        S[i].w = 200 + ((seed >> 12) % 600);
        S[i].h = 150 + ((seed >> 16) % 400);
        S[i].z = i;
        S[i].layer = (i == 0) ? 0 : 1;
        S[i].opaque = (i % 3) != 0;
        S[i].visible = 1;
        ORDER[i] = i;
    }
    qsort(ORDER, NS, sizeof(int), cmp_rank);

    Rect dmg[50];
    for (int i = 0; i < 50; i++) {
        seed = seed * 1103515245u + 12345u;
        dmg[i] = rect((seed >> 8) % 1900, (seed >> 4) % 1000, 40 + (seed % 200), 40 + ((seed >> 3) % 160));
    }

    long total_px = 0;
    int frames = 500;
    double t = now();
    for (int f = 0; f < frames; f++) {
        long px = 0;
        for (int i = 0; i < 50; i++) px += collect(dmg[i]);
        total_px += px;
    }
    double dt = now() - t;
    printf("C(-O2)  %d surfaces / 50 damage / %d 幀\n", NS, frames);
    printf("  總計 %8.1f ms ｜ 每幀 %6.3f ms ｜ 平均像素 %.0f K\n",
           dt, dt / frames, (double)total_px / frames / 1000.0);
    return 0;
}
