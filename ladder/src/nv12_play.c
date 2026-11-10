/* nv12_play — ffmpeg 解碼 → NV12 → KMS 直掃
 *
 * stdin 吃 `-f rawvideo -pix_fmt nv12` 的 1920x1080 幀（3,110,400 B/幀），
 * 讀執行緒填進 3 個 GBM NV12 掃描緩衝，主執行緒以 120 Hz page flip。
 * 沒有新幀時重翻目前緩衝（同一幀重複顯示）。
 *
 * 用法：ffmpeg ... -f rawvideo - | sudo nv12_play [card] [hz]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define W 1920
#define H 1080
#define NB 3
#define FRAME_SZ ((size_t)W * H * 3 / 2)

typedef struct {
    struct gbm_bo *bo;
    uint32_t fb;
    uint8_t *base;
    uint32_t ys, yo, cs, co;
} VB;

static VB V[NB];
static volatile uint32_t seq[NB], ack[NB];
static volatile int eof_flag = 0;

static volatile int flipped = 0;
static void on_event(int fd, unsigned s, unsigned a, unsigned b, void *d) {
    (void)fd;(void)s;(void)a;(void)b;(void)d; flipped = 1;
}
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static drmModeConnector *pick(int fd, drmModeRes **ro) {
    drmModeRes *r = drmModeGetResources(fd);
    if (!r) return NULL;
    *ro = r;
    for (int i = 0; i < r->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, r->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) return c;
        drmModeFreeConnector(c);
    }
    return NULL;
}

/* ── 讀執行緒：stdin → 空閒掃描緩衝（做 stride 轉換） ── */
static void *reader(void *arg) {
    (void)arg;
    uint8_t *tmp = malloc(FRAME_SZ);
    if (!tmp) { eof_flag = 1; return NULL; }
    int i = 0;
    while (!eof_flag) {
        while (!eof_flag &&
               __atomic_load_n(&seq[i], __ATOMIC_ACQUIRE) !=
               __atomic_load_n(&ack[i], __ATOMIC_ACQUIRE))
            usleep(500);
        if (eof_flag) break;

        size_t got = fread(tmp, 1, FRAME_SZ, stdin);
        if (got < FRAME_SZ) { eof_flag = 1; break; }

        VB *v = &V[i];
        for (int y = 0; y < H; y++)
            memcpy(v->base + v->yo + (size_t)y * v->ys, tmp + (size_t)y * W, W);
        for (int y = 0; y < H / 2; y++)
            memcpy(v->base + v->co + (size_t)y * v->cs,
                   tmp + (size_t)W * H + (size_t)y * W, W);

        __atomic_add_fetch(&seq[i], 1, __ATOMIC_RELEASE);
        i = (i + 1) % NB;
    }
    free(tmp);
    return NULL;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *card = argc > 1 ? argv[1] : "/dev/dri/card2";
    int hz = argc > 2 ? atoi(argv[2]) : 120;

    int fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }
    if (drmSetMaster(fd)) fprintf(stderr, "drmSetMaster: %s\n", strerror(errno));

    drmModeRes *res = NULL;
    drmModeConnector *conn = pick(fd, &res);
    if (!conn) { fprintf(stderr, "no connector\n"); return 1; }
    drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
    uint32_t crtc = enc ? enc->crtc_id : 0;
    if (!crtc) for (int i = 0; i < res->count_crtcs; i++) { crtc = res->crtcs[i]; break; }
    drmModeModeInfo mode = conn->modes[0];
    int best = -1000;
    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo *m = &conn->modes[i];
        if (m->hdisplay != W || m->vdisplay != H) continue;
        if (m->flags & DRM_MODE_FLAG_INTERLACE) continue;
        int sc = hz ? -abs((int)m->vrefresh - hz) : (int)m->vrefresh;
        if (sc > best) { best = sc; mode = *m; }
    }
    printf("nv12_play: %dx%d@%d crtc=%u\n", mode.hdisplay, mode.vdisplay, mode.vrefresh, crtc);

    struct gbm_device *g = gbm_create_device(fd);
    if (!g) { perror("gbm"); return 1; }

    for (int i = 0; i < NB; i++) {
        V[i].bo = gbm_bo_create(g, W, H, GBM_FORMAT_NV12,
                                GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE);
        if (!V[i].bo) { perror("gbm_bo_create"); return 1; }
        V[i].ys = gbm_bo_get_stride_for_plane(V[i].bo, 0);
        V[i].yo = gbm_bo_get_offset(V[i].bo, 0);
        V[i].cs = gbm_bo_get_stride_for_plane(V[i].bo, 1);
        V[i].co = gbm_bo_get_offset(V[i].bo, 1);
        size_t sz = (size_t)V[i].co + (size_t)V[i].cs * (H / 2);
        int dfd = gbm_bo_get_fd(V[i].bo);
        V[i].base = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
        if (V[i].base == MAP_FAILED) { perror("mmap"); return 1; }
        memset(V[i].base, 16, sz);   /* 先填黑（Y=16, UV=128 之後覆蓋） */

        uint32_t h[4] = {gbm_bo_get_handle(V[i].bo).u32, gbm_bo_get_handle(V[i].bo).u32, 0, 0};
        uint32_t p[4] = {V[i].ys, V[i].cs, 0, 0};
        uint32_t o[4] = {V[i].yo, V[i].co, 0, 0};
        if (drmModeAddFB2(fd, W, H, DRM_FORMAT_NV12, h, p, o, &V[i].fb, 0)) {
            perror("AddFB2"); return 1;
        }
    }

    if (drmModeSetCrtc(fd, crtc, V[0].fb, 0, 0, &conn->connector_id, 1, &mode)) {
        perror("SetCrtc"); return 1;
    }

    pthread_t th;
    pthread_create(&th, NULL, reader, NULL);

    /* 等第一幀 */
    double wait0 = now_ms();
    while (__atomic_load_n(&seq[0], __ATOMIC_ACQUIRE) == 0 &&
           __atomic_load_n(&seq[1], __ATOMIC_ACQUIRE) == 0 &&
           __atomic_load_n(&seq[2], __ATOMIC_ACQUIRE) == 0) {
        if (eof_flag) break;
        if (now_ms() - wait0 > 5000) break;
        usleep(1000);
    }

    int cur = -1, pending = -1, pending_old = -1;
    double t0 = now_ms(), last = t0, cpu = 0;
    double expected = 1000.0 / mode.vrefresh;
    int drops = 0, shown = 0, video_frames = 0;
    double idle_since = 0;

    for (;;) {
        double c0 = now_ms();
        int b = -1;
        for (int i = 0; i < NB; i++) {
            if (i == cur || i == pending) continue;
            if (__atomic_load_n(&seq[i], __ATOMIC_ACQUIRE) !=
                __atomic_load_n(&ack[i], __ATOMIC_ACQUIRE)) { b = i; break; }
        }
        int old = -1;
        if (b < 0) {
            if (cur < 0) { usleep(500); continue; }
            b = cur;
            if (eof_flag) {
                if (idle_since == 0) idle_since = now_ms();
                else if (now_ms() - idle_since > 500) break;   /* 播完再停 0.5s */
            }
        } else {
            old = cur;
            video_frames++;
            idle_since = 0;
        }
        cpu += now_ms() - c0;

        if (drmModePageFlip(fd, crtc, V[b].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL)) {
            perror("PageFlip"); break;
        }
        pending = b; pending_old = old;
        flipped = 0;
        struct pollfd pf = {fd, POLLIN, 0};
        int gd = 0;
        while (!flipped && gd++ < 200) {
            if (poll(&pf, 1, 100) > 0) {
                drmEventContext ev = {0}; ev.version = 2; ev.page_flip_handler = on_event;
                drmHandleEvent(fd, &ev);
            }
        }
        if (pending_old >= 0)
            __atomic_store_n(&ack[pending_old],
                             __atomic_load_n(&seq[pending_old], __ATOMIC_ACQUIRE),
                             __ATOMIC_RELEASE);
        cur = pending; pending = -1;
        shown++;

        double now = now_ms(), gap = now - last;
        if (gap > expected * 1.5) drops += (int)(gap / expected + 0.5) - 1;
        last = now;
    }

    eof_flag = 1;
    pthread_join(th, NULL);
    double dt = now_ms() - t0;
    printf("顯示 %d 幀 / %.1f ms → %.1f fps（理論 %d Hz）\n",
           shown, dt, shown * 1000.0 / dt, mode.vrefresh);
    printf("影片 %d 幀 ｜ 主迴圈 CPU %.3f ms/幀 ｜ 漏拍 %d\n",
           video_frames, cpu / (shown ? shown : 1), drops);
    return 0;
}
