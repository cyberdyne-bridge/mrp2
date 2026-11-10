/* ════════════ KMS 輸出端（三緩衝 + 協議 DAMAGE 同步） ════════════
 *
 * 前段是原 csrc/mrpd.c（解碼／plan／raster）。
 * 這裡把 FB 接到 DRM/KMS：每個緩衝只同步「協議髒區」，然後 page flip。
 */
#include <errno.h>
#include <poll.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define NBUF 3
#define MAX_BDMG 256

typedef struct {
    uint32_t fb, handle;
    uint64_t size;
    uint32_t pitch;
    uint8_t *map;
} Kbuf;

static Kbuf KB[NBUF];
static int current = 0, pending = -1;
static volatile int flipped = 0;
static int SWAP_RB = 0;          /* 若無法用 XBGR8888，同步時逐像素換 R/B */
static int OPT_FULLSYNC = 0;     /* --full：每幀整屏同步（對照用） */

static void on_event(int fd, unsigned seq, unsigned sec, unsigned usec, void *data) {
    (void)fd; (void)seq; (void)sec; (void)usec; (void)data;
    flipped = 1;
}

typedef struct { Rect r[MAX_BDMG]; int n; } DmgList;
static DmgList BD[NBUF];

static void bd_add(DmgList *d, Rect r) {
    if (empty(r)) return;
    if (d->n < MAX_BDMG) { d->r[d->n++] = r; return; }
    Rect b = d->r[0];
    for (int i = 1; i < d->n; i++) b = uni(b, d->r[i]);
    d->r[0] = uni(b, r);
    d->n = 1;
}

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

/* 把這個緩衝累積的髒區從 FB 同步過去 */
static void sync_buf(int bi, double *ms) {
    double t = now_ms();
    Kbuf *k = &KB[bi];
    DmgList *d = &BD[bi];
    for (int i = 0; i < d->n; i++) {
        Rect r = d->r[i];
        int x0 = r.x < 0 ? 0 : r.x;
        int y0 = r.y < 0 ? 0 : r.y;
        int x1 = r.x + r.w; if (x1 > W) x1 = W;
        int y1 = r.y + r.h; if (y1 > H) y1 = H;
        if (x1 <= x0 || y1 <= y0) continue;
        size_t len = (size_t)(x1 - x0) * 4;
        for (int y = y0; y < y1; y++) {
            uint8_t *dst = k->map + (size_t)y * k->pitch + (size_t)x0 * 4;
            const uint8_t *src = (uint8_t *)FB + (size_t)y * W * 4 + (size_t)x0 * 4;
            if (!SWAP_RB) {
                memcpy(dst, src, len);
            } else {
                uint32_t *dd = (uint32_t *)dst;
                const uint32_t *ss = (const uint32_t *)src;
                for (int x = 0; x < x1 - x0; x++)
                    dd[x] = (ss[x] & 0xFF00FF00u) | ((ss[x] & 0xFFu) << 16) |
                            ((ss[x] >> 16) & 0xFFu);
            }
        }
    }
    d->n = 0;
    *ms = now_ms() - t;
}

static void wait_flip(int fd) {
    flipped = 0;
    struct pollfd pfd = {fd, POLLIN, 0};
    int guard = 0;
    while (!flipped && guard++ < 200) {
        if (poll(&pfd, 1, 100) > 0) {
            drmEventContext ev = {0};
            ev.version = 2;
            ev.page_flip_handler = on_event;
            drmHandleEvent(fd, &ev);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "用法：mrpd_kms scene.wire [幀數] [Hz] [--font path] [--card /dev/dri/cardN] [--full]\n");
        return 2;
    }
    const char *wire = argv[1];
    int frames = argc > 2 ? atoi(argv[2]) : 600;
    int hz = argc > 3 ? atoi(argv[3]) : 120;
    const char *font_path = NULL;
    const char *card = "/dev/dri/card2";
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
        else if (!strcmp(argv[i], "--full")) OPT_FULLSYNC = 1;
    }
    if (frames <= 0) frames = 600;

    W = 1920;
    H = 1080;
    if (font_path && !load_font(font_path)) return 1;
    FB = malloc((size_t)W * H * 4);
    if (!FB) { fprintf(stderr, "malloc FB 失敗\n"); return 1; }

    double t_dec = 0;
    if (!load_wire(wire, &t_dec)) return 1;
    for (int i = 0; i < NS; i++) ORDER[i] = i;
    qsort(ORDER, NS, sizeof(int), cmp_rank);

    double t_plan = 0, t_rast = 0;
    plan(&t_plan);
    raster(&t_rast);
    int spans = 0;
    for (int i = 0; i < NS; i++) spans += S[i].nsp;
    printf("解碼 %.2f ms ｜ %d surface ／ %d span\n", t_dec, NS, spans);
    printf("首幀：plan %.2f ms ／ raster %.2f ms ／ ops %d\n", t_plan, t_rast, NO);

    /* ── 開 DRM ── */
    int fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open card"); return 1; }
    if (drmSetMaster(fd) != 0)
        fprintf(stderr, "警告：drmSetMaster 失敗：%s\n", strerror(errno));

    drmModeRes *res = NULL;
    drmModeConnector *conn = pick_connector(fd, &res);
    if (!conn) { fprintf(stderr, "找不到已連接的螢幕\n"); return 1; }
    drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
    uint32_t crtc = enc ? enc->crtc_id : 0;
    if (!crtc)
        for (int i = 0; i < res->count_crtcs; i++) { crtc = res->crtcs[i]; break; }

    drmModeModeInfo mode = conn->modes[0];
    int best = -1000;
    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo *m = &conn->modes[i];
        if (m->hdisplay != W || m->vdisplay != H) continue;
        if (m->flags & DRM_MODE_FLAG_INTERLACE) continue;
        int score = hz ? -abs((int)m->vrefresh - hz) : (int)m->vrefresh;
        if (score > best) { best = score; mode = *m; }
    }
    printf("KMS：connector=%d crtc=%u mode=%dx%d@%d  buffer=%.1f MB ×%d\n",
           conn->connector_id, crtc, mode.hdisplay, mode.vdisplay, mode.vrefresh,
           W * H * 4.0 / 1048576.0, NBUF);
    if (enc) drmModeFreeEncoder(enc);

    /* ── 三個 dumb buffer（FB 是 XBGR8888，直接 AddFB2，免轉換） ── */
    int use_xbgr = 1;
    for (int i = 0; i < NBUF; i++) {
        struct drm_mode_create_dumb cd = {0};
        cd.width = W; cd.height = H; cd.bpp = 32;
        if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) {
            perror("CREATE_DUMB"); return 1;
        }
        KB[i].handle = cd.handle;
        KB[i].pitch = cd.pitch;
        KB[i].size = cd.size;

        int ok = 0;
        if (use_xbgr) {
            uint32_t h4[4] = {cd.handle, 0, 0, 0};
            uint32_t p4[4] = {cd.pitch, 0, 0, 0};
            uint32_t o4[4] = {0, 0, 0, 0};
            if (drmModeAddFB2(fd, W, H, DRM_FORMAT_XBGR8888, h4, p4, o4, &KB[i].fb, 0) == 0)
                ok = 1;
            else if (i == 0)
                use_xbgr = 0;
        }
        if (!ok) {
            if (drmModeAddFB(fd, W, H, 24, 32, cd.pitch, cd.handle, &KB[i].fb)) {
                perror("AddFB"); return 1;
            }
        }

        struct drm_mode_map_dumb md = {0};
        md.handle = cd.handle;
        if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("map dumb"); return 1; }
        KB[i].map = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
        if (KB[i].map == MAP_FAILED) { perror("mmap"); return 1; }
    }
    SWAP_RB = !use_xbgr;
    if (SWAP_RB)
        printf("注意：不支援 XBGR8888，改用 XRGB + 同步時逐像素換色\n");
    else
        printf("像素格式：XBGR8888（與 mrpd FB 相同，同步時零轉換）\n");

    /* 初次把整張 FB 灌進三個緩衝 */
    double dummy = 0;
    for (int i = 0; i < NBUF; i++) {
        BD[i].n = 0;
        bd_add(&BD[i], (Rect){0, 0, W, H});
        sync_buf(i, &dummy);
    }
    if (drmModeSetCrtc(fd, crtc, KB[0].fb, 0, 0, &conn->connector_id, 1, &mode)) {
        perror("SetCrtc"); return 1;
    }

    /* ── 主迴圈：內容更新 → plan/raster → 髒區同步 → page flip ── */
    double t0 = now_ms();
    double last_complete = t0;
    double expected = 1000.0 / (mode.vrefresh ? mode.vrefresh : 60);
    double cpu_total = 0, sync_total = 0;
    int drops = 0;

    for (int f = 1; f <= frames; f++) {
        double c0 = now_ms();

        /* 模擬內容更新：每幀只改一條 16px 文字帶（協議只會看到這塊髒區） */
        ND = 0;
        NO = 0;
        for (int i = 0; i < NS; i++)
            if (S[i].role != 0 && S[i].visible)
                add_damage((Rect){S[i].x, S[i].y + 22 + (f % 20) * 16, S[i].w, 16});
        plan(&t_plan);
        raster(&t_rast);

        /* 本幀髒區累加到每個緩衝（三緩衝各隔 3 幀才重用） */
        for (int i = 0; i < NBUF; i++) {
            if (OPT_FULLSYNC) {
                bd_add(&BD[i], (Rect){0, 0, W, H});
            } else {
                for (int d = 0; d < ND; d++) bd_add(&BD[i], DMG[d]);
            }
        }

        int b = -1;
        for (int i = 0; i < NBUF; i++)
            if (i != current && i != pending) { b = i; break; }
        if (b < 0) b = (current + 1) % NBUF;

        double s = 0;
        sync_buf(b, &s);
        sync_total += s;
        cpu_total += now_ms() - c0;

        if (pending >= 0) {
            wait_flip(fd);
            current = pending;
            pending = -1;
            double now = now_ms(), gap = now - last_complete;
            if (gap > expected * 1.5) drops += (int)(gap / expected + 0.5) - 1;
            last_complete = now;
        }

        if (drmModePageFlip(fd, crtc, KB[b].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL)) {
            perror("PageFlip"); break;
        }
        pending = b;
    }
    if (pending >= 0) wait_flip(fd);

    double dt = now_ms() - t0;
    printf("%d 幀 / %.1f ms → %.1f fps（理論 %d Hz）\n",
           frames, dt, frames * 1000.0 / dt, mode.vrefresh);
    printf("CPU %.3f ms/幀（其中髒區同步 %.3f ms）｜ 漏拍 %d 幀\n",
           cpu_total / frames, sync_total / frames, drops);
    return 0;
}
