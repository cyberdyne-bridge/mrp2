/* kms.c — 直接吃 DRM/KMS：dumb buffer → modeset → page flip 60 Hz
 *
 * 這支程式不經過 X11、不經過 Wayland：自己當 display server 的輸出端。
 * CPU 渲染（i7-6700 先跑）之後再把同一塊 buffer 換成 GPU 產物。
 *
 *   gcc -O2 -o kms kms.c -I/usr/include/libdrm -ldrm
 *   sudo ./kms [card]        # 預設 /dev/dri/card0
 *
 * 注意：需要 root 或 video 群組；執行時要離開圖形介面（拿到 DRM master）。
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

static volatile int flipped = 0;

static void on_event(int fd, unsigned seq, unsigned sec, unsigned usec, void *data) {
    (void)fd; (void)seq; (void)sec; (void)usec; (void)data;
    flipped = 1;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* 找一個有接螢幕的 connector，回傳它建議的模式 */
static drmModeConnector *pick_connector(int fd, drmModeRes **res_out) {
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) return NULL;
    *res_out = res;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
            return c;
        drmModeFreeConnector(c);
    }
    return NULL;
}

static void draw(uint8_t *buf, int w, int h, int pitch, int frame) {
    for (int y = 0; y < h; y++) {
        uint8_t *row = buf + (size_t)y * pitch;
        for (int x = 0; x < w; x++) {
            uint8_t *p = row + (size_t)x * 4;
            int bar = (frame * 6) % (w + 200) - 100;
            int on = x >= bar && x < bar + 120 && y > h / 3 && y < h * 2 / 3;
            p[0] = on ? 0xF0 : (uint8_t)(x * 255 / w);         /* B */
            p[1] = on ? 0xA0 : (uint8_t)(y * 255 / h);         /* G */
            p[2] = on ? 0x20 : 0x18;                            /* R */
            p[3] = 0;
        }
    }
}


/* ── 快速版：背景預計算 + 每幀 memcpy + 只畫移動豎條 ───────────── */
static void fill_bg(uint8_t *buf, int w, int h, int pitch) {
    uint8_t *bx = malloc((size_t)w);
    for (int x = 0; x < w; x++) bx[x] = (uint8_t)(x * 255 / w);
    for (int y = 0; y < h; y++) {
        uint8_t *row = buf + (size_t)y * pitch;
        uint8_t g = (uint8_t)(y * 255 / h);
        for (int x = 0; x < w; x++) {
            uint8_t *p = row + (size_t)x * 4;
            p[0] = bx[x]; p[1] = g; p[2] = 0x18; p[3] = 0;
        }
    }
    free(bx);
}

static void draw_bar(uint8_t *buf, int w, int h, int pitch, int bar) {
    int x0 = bar < 0 ? 0 : bar;
    int x1 = bar + 120; if (x1 > w) x1 = w;
    if (x1 <= x0) return;
    int y0 = h / 3, y1 = h * 2 / 3;
    uint32_t px = 0x0020A0F0u;   /* BGRA: B=F0 G=A0 R=20 A=00 */
    for (int y = y0; y < y1; y++) {
        uint32_t *row = (uint32_t *)(buf + (size_t)y * pitch);
        for (int x = x0; x < x1; x++) row[x] = px;
    }
}

int main(int argc, char **argv) {
    const char *card = argc > 1 ? argv[1] : "/dev/dri/card0";
    int fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open card"); return 1; }
    if (drmSetMaster(fd) != 0) fprintf(stderr, "警告：drmSetMaster 失敗（可能不是 DRM master）\n");

    drmModeRes *res = NULL;
    drmModeConnector *conn = pick_connector(fd, &res);
    if (!conn) { fprintf(stderr, "找不到已連接的螢幕\n"); return 1; }
    drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
    uint32_t crtc = enc ? enc->crtc_id : 0;
    if (!crtc) {
        for (int i = 0; i < res->count_crtcs; i++) { crtc = res->crtcs[i]; break; }
    }
    /* 挑 1920×1080 非交錯；給了目標刷新率就選最接近的，否則選最高的 */
    int want_hz = argc > 2 ? atoi(argv[2]) : 0;
    drmModeModeInfo mode = conn->modes[0];
    int best_score = -1000;
    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo *m = &conn->modes[i];
        if (m->hdisplay != 1920 || m->vdisplay != 1080) continue;
        if (m->flags & DRM_MODE_FLAG_INTERLACE) continue;
        int score = want_hz ? -abs((int)m->vrefresh - want_hz) : (int)m->vrefresh;
        if (score > best_score) { best_score = score; mode = *m; }
    }
    if (want_hz && mode.vrefresh != (unsigned)want_hz)
        fprintf(stderr, "注意：螢幕沒有 %d Hz，用最接近的 %d Hz\n", want_hz, mode.vrefresh);
    printf("connector=%d crtc=%u mode=%dx%d@%d  buffer=%.1f MB ×2\n",
           conn->connector_id, crtc, mode.hdisplay, mode.vdisplay, mode.vrefresh,
           mode.hdisplay * mode.vdisplay * 4.0 / 1048576.0);
    if (enc) drmModeFreeEncoder(enc);

    /* dumb buffer ×2（雙緩衝，page flip 才不會撕裂） */
    struct { uint32_t fb, handle; uint64_t size; uint32_t pitch; uint8_t *map; } buf[2];
    for (int i = 0; i < 2; i++) {
        struct drm_mode_create_dumb cd = {0};
        cd.width = mode.hdisplay; cd.height = mode.vdisplay; cd.bpp = 32;
        if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) {
            fprintf(stderr, "CREATE_DUMB 失敗（nvidia 專有驅動常見）：%s\n", strerror(errno));
            fprintf(stderr, "→ 換路：GBM/EGL 配置 buffer，再 drmModeAddFB2\n");
            return 1;
        }
        buf[i].handle = cd.handle; buf[i].pitch = cd.pitch; buf[i].size = cd.size;
        if (drmModeAddFB(fd, cd.width, cd.height, 24, 32, cd.pitch, cd.handle, &buf[i].fb)) {
            fprintf(stderr, "AddFB 失敗\n"); return 1;
        }
        struct drm_mode_map_dumb md = {0};
        md.handle = cd.handle;
        if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("map dumb"); return 1; }
        buf[i].map = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
        if (buf[i].map == MAP_FAILED) { perror("mmap"); return 1; }
    }
    uint8_t *bg = malloc(buf[0].size);
    if (!bg) { fprintf(stderr, "malloc bg 失敗\n"); return 1; }
    fill_bg(bg, mode.hdisplay, mode.vdisplay, buf[0].pitch);
    memcpy(buf[0].map, bg, buf[0].size);
    draw_bar(buf[0].map, mode.hdisplay, mode.vdisplay, buf[0].pitch, 0);
    if (drmModeSetCrtc(fd, crtc, buf[0].fb, 0, 0, &conn->connector_id, 1, &mode)) {
        perror("SetCrtc"); return 1;
    }

    /* 60 幀：畫 → page flip → 等 FLIP_COMPLETE */
    double t0 = now_ms();
    double compose_ms = 0;
    int N = 120;
    for (int f = 1; f <= N; f++) {
        int cur = f & 1;
        double c0 = now_ms();
        memcpy(buf[cur].map, bg, buf[cur].size);
        draw_bar(buf[cur].map, mode.hdisplay, mode.vdisplay, buf[cur].pitch,
                 (f * 6) % (mode.hdisplay + 200) - 100);
        compose_ms += now_ms() - c0;
        flipped = 0;
        if (drmModePageFlip(fd, crtc, buf[cur].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL)) {
            perror("PageFlip"); break;
        }
        struct pollfd pfd = {fd, POLLIN, 0};
        int guard = 0;
        while (!flipped && guard++ < 100) {
            if (poll(&pfd, 1, 100) > 0) {
                drmEventContext ev = {0};
                ev.version = 2; ev.page_flip_handler = on_event;
                drmHandleEvent(fd, &ev);
            }
        }
    }
    double dt = now_ms() - t0;
    printf("%d 幀 / %.1f ms → %.1f fps（理論 %d Hz）\n", N, dt, N * 1000.0 / dt, mode.vrefresh);
    printf("每幀合成（memcpy+豎條）：%.3f ms\n", compose_ms / N);
    return 0;
}
