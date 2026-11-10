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

#define NBUF 3   /* 三緩衝 */

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
    /* SMPTE 風格八色豎條（記憶體順序 B,G,R,A） */
    static const uint32_t bars[8] = {
        0x00FFFFFFu, /* white   */
        0x00FFFF00u, /* yellow  */
        0x0000FFFFu, /* cyan    */
        0x0000FF00u, /* green   */
        0x00FF00FFu, /* magenta */
        0x00FF0000u, /* red     */
        0x000000FFu, /* blue    */
        0x00000000u, /* black   */
    };
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(buf + (size_t)y * pitch);
        for (int x = 0; x < w; x++) {
            int b = x * 8 / w;
            if (b > 7) b = 7;
            row[x] = bars[b];
        }
    }
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

/* 只把「舊豎條 ∪ 新豎條」的列從背景還原，再畫新豎條（窄帶重繪） */
static void damage_update(uint8_t *dst, const uint8_t *bg, int w, int h,
                          int pitch, int oldbar, int newbar) {
    int a0 = oldbar < newbar ? oldbar : newbar;
    int a1 = (oldbar > newbar ? oldbar : newbar) + 120;
    int x0 = a0 < 0 ? 0 : a0;
    int x1 = a1 > w ? w : a1;
    if (x1 <= x0) return;
    int y0 = h / 3, y1 = h * 2 / 3;
    size_t len = (size_t)(x1 - x0) * 4;
    for (int y = y0; y < y1; y++) {
        uint8_t *d = dst + (size_t)y * pitch + (size_t)x0 * 4;
        const uint8_t *s = bg + (size_t)y * pitch + (size_t)x0 * 4;
        memcpy(d, s, len);
    }
    draw_bar(dst, w, h, pitch, newbar);
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
    printf("connector=%d crtc=%u mode=%dx%d@%d  buffer=%.1f MB ×%d\n",
           conn->connector_id, crtc, mode.hdisplay, mode.vdisplay, mode.vrefresh,
           mode.hdisplay * mode.vdisplay * 4.0 / 1048576.0, NBUF);
    if (enc) drmModeFreeEncoder(enc);

    /* dumb buffer ×3（三緩衝：掃描 / pending / 可寫） */
    struct { uint32_t fb, handle; uint64_t size; uint32_t pitch; uint8_t *map; } buf[NBUF];
    for (int i = 0; i < NBUF; i++) {
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
    int last[NBUF];
    for (int i = 0; i < NBUF; i++) {
        memcpy(buf[i].map, bg, buf[i].size);
        draw_bar(buf[i].map, mode.hdisplay, mode.vdisplay, buf[i].pitch, 0);
        last[i] = 0;
    }
    if (drmModeSetCrtc(fd, crtc, buf[0].fb, 0, 0, &conn->connector_id, 1, &mode)) {
        perror("SetCrtc"); return 1;
    }

    /* 三緩衝：render → 等上一張 FLIP_COMPLETE → 提交 → 回收 */
    double t0 = now_ms();
    double compose_ms = 0;
    int N = 1800;   /* 預設 15 秒 @120Hz；可用 argv[3] 指定幀數 */
    if (argc > 3) { int v = atoi(argv[3]); if (v > 0) N = v; }
    int current = 0;      /* 正在掃描輸出的 buffer */
    int pending = -1;     /* 已提交、等 FLIP_COMPLETE 的 buffer */
    int drops = 0;
    double expected = 1000.0 / (mode.vrefresh ? mode.vrefresh : 60);
    double last_complete = t0;

    for (int f = 1; f <= N; f++) {
        int b = -1;
        for (int i = 0; i < NBUF; i++)
            if (i != current && i != pending) { b = i; break; }
        if (b < 0) b = (current + 1) % NBUF;

        int nb = (f * 6) % (mode.hdisplay + 200) - 100;
        double c0 = now_ms();
        damage_update(buf[b].map, bg, mode.hdisplay, mode.vdisplay,
                      buf[b].pitch, last[b], nb);
        last[b] = nb;
        compose_ms += now_ms() - c0;

        /* 上一張 flip 完成 → 它取代掉的那張 buffer 就自由了 */
        if (pending >= 0) {
            flipped = 0;
            struct pollfd pfd = {fd, POLLIN, 0};
            int guard = 0;
            while (!flipped && guard++ < 200) {
                if (poll(&pfd, 1, 100) > 0) {
                    drmEventContext ev = {0};
                    ev.version = 2; ev.page_flip_handler = on_event;
                    drmHandleEvent(fd, &ev);
                }
            }
            current = pending;
            pending = -1;
            double now = now_ms();
            double gap = now - last_complete;
            if (gap > expected * 1.5)
                drops += (int)(gap / expected + 0.5) - 1;
            last_complete = now;
        }

        if (drmModePageFlip(fd, crtc, buf[b].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL)) {
            perror("PageFlip"); break;
        }
        pending = b;
    }
    /* 等最後一張完成 */
    if (pending >= 0) {
        flipped = 0;
        struct pollfd pfd = {fd, POLLIN, 0};
        int guard = 0;
        while (!flipped && guard++ < 200) {
            if (poll(&pfd, 1, 100) > 0) {
                drmEventContext ev = {0};
                ev.version = 2; ev.page_flip_handler = on_event;
                drmHandleEvent(fd, &ev);
            }
        }
    }
    double dt = now_ms() - t0;
    printf("%d 幀 / %.1f ms → %.1f fps（理論 %d Hz）\n", N, dt, N * 1000.0 / dt, mode.vrefresh);
    printf("每幀合成（DAMAGE 窄帶）：%.3f ms\n", compose_ms / N);
    printf("三緩衝｜漏拍（>1.5× vsync）：%d 幀\n", drops);
    return 0;
}
