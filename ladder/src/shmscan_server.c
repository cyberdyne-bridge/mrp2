/* shmscan_server — 零拷貝影片直掃（伺服端）
 *
 * 建立 3 個 KMS dumb buffer，透過 Unix socket + SCM_RIGHTS 把 fd 交給生產端；
 * 生產端直接寫掃描緩衝，伺服器只負責 page flip。
 *
 * 用法：sudo shmscan_server [card] [hz] [frames] [socket]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <drm_fourcc.h>

#define NBUF 3
#define W 1920
#define H 1080

typedef struct {
    volatile uint32_t seq[NBUF];
    volatile uint32_t ack[NBUF];
} Ctrl;

typedef struct { uint32_t w, h, pitch, nbuf; } Hdr;

typedef struct { uint32_t fb, handle; uint64_t size; uint32_t pitch; uint8_t *map; } Kbuf;
static Kbuf KB[NBUF];
static volatile int flipped = 0;

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static void on_event(int fd, unsigned seq, unsigned sec, unsigned usec, void *d) {
    (void)fd;(void)seq;(void)sec;(void)usec;(void)d; flipped = 1;
}
static void wait_flip(int fd) {
    flipped = 0;
    struct pollfd p = {fd, POLLIN, 0};
    int g = 0;
    while (!flipped && g++ < 200) {
        if (poll(&p, 1, 100) > 0) {
            drmEventContext ev = {0}; ev.version = 2; ev.page_flip_handler = on_event;
            drmHandleEvent(fd, &ev);
        }
    }
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

int main(int argc, char **argv) {
    const char *card = argc > 1 ? argv[1] : "/dev/dri/card2";
    int hz = argc > 2 ? atoi(argv[2]) : 120;
    int frames = argc > 3 ? atoi(argv[3]) : 600;
    const char *sockpath = argc > 4 ? argv[4] : "/tmp/mrp-scan.sock";

    int fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open card"); return 1; }
    if (drmSetMaster(fd) != 0) fprintf(stderr, "警告：drmSetMaster：%s\n", strerror(errno));

    drmModeRes *res = NULL;
    drmModeConnector *conn = pick(fd, &res);
    if (!conn) { fprintf(stderr, "找不到螢幕\n"); return 1; }
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
    printf("KMS mode=%dx%d@%d\n", mode.hdisplay, mode.vdisplay, mode.vrefresh);
    if (enc) drmModeFreeEncoder(enc);

    struct gbm_device *g = gbm_create_device(fd);
    if (!g) { perror("gbm_create_device"); return 1; }
    printf("GBM backend: %s\n", gbm_device_get_backend_name(g));

    int exp_fd[1 + NBUF];
    struct gbm_bo *bos[NBUF];
    for (int i = 0; i < NBUF; i++) {
        bos[i] = gbm_bo_create(g, W, H, GBM_FORMAT_XBGR8888,
                               GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE);
        if (!bos[i]) { perror("gbm_bo_create"); return 1; }
        KB[i].handle = gbm_bo_get_handle(bos[i]).u32;
        KB[i].pitch = gbm_bo_get_stride(bos[i]);
        KB[i].size = (uint64_t)KB[i].pitch * H;
        uint32_t h4[4] = {KB[i].handle,0,0,0}, p4[4] = {KB[i].pitch,0,0,0}, o4[4] = {0,0,0,0};
        if (drmModeAddFB2(fd, W, H, DRM_FORMAT_XBGR8888, h4, p4, o4, &KB[i].fb, 0)) {
            perror("AddFB2"); return 1;
        }
        exp_fd[i + 1] = gbm_bo_get_fd(bos[i]);
        if (exp_fd[i + 1] < 0) { perror("gbm_bo_get_fd"); return 1; }
    }

    int ctrl_fd = memfd_create("mrp-ctrl", MFD_CLOEXEC);
    if (ctrl_fd < 0) { perror("memfd"); return 1; }
    if (ftruncate(ctrl_fd, sizeof(Ctrl))) { perror("ftruncate"); return 1; }
    Ctrl *c = mmap(NULL, sizeof(Ctrl), PROT_READ | PROT_WRITE, MAP_SHARED, ctrl_fd, 0);
    if (c == MAP_FAILED) { perror("mmap ctrl"); return 1; }
    memset(c, 0, sizeof(Ctrl));
    exp_fd[0] = ctrl_fd;

    unlink(sockpath);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, sockpath, sizeof(sa.sun_path) - 1);
    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) || listen(srv, 1)) {
        perror("bind/listen"); return 1;
    }
    chmod(sockpath, 0777);
    printf("等待生產端連線：%s\n", sockpath);
    int cli = accept(srv, NULL, NULL);
    if (cli < 0) { perror("accept"); return 1; }

    Hdr h = {W, H, KB[0].pitch, NBUF};
    struct iovec iov = {&h, sizeof(h)};
    char cbuf[CMSG_SPACE(sizeof(int) * 4)];
    struct msghdr msg = {0};
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int) * 4);
    memcpy(CMSG_DATA(cm), exp_fd, sizeof(int) * 4);
    if (sendmsg(cli, &msg, 0) < 0) { perror("sendmsg"); return 1; }
    printf("已交付 %d 個緩衝 fd（%dx%d pitch=%u）\n", NBUF, W, H, h.pitch);

    if (drmModeSetCrtc(fd, crtc, KB[0].fb, 0, 0, &conn->connector_id, 1, &mode)) {
        perror("SetCrtc"); return 1;
    }

    int cur = -1, pend = -1, pend_old = -1;
    double t0 = now_ms(), last = t0, expected = 1000.0 / mode.vrefresh;
    double cpu = 0;
    int drops = 0, shown = 0;

    for (int f = 0; f < frames; f++) {
        double c0 = now_ms();
        int b = -1;
        for (int i = 0; i < NBUF; i++) {
            if (i == cur || i == pend) continue;
            uint32_t s = __atomic_load_n(&c->seq[i], __ATOMIC_ACQUIRE);
            uint32_t a = __atomic_load_n(&c->ack[i], __ATOMIC_ACQUIRE);
            if (s != a) { b = i; break; }
        }
        int old = -1;
        if (b < 0) { b = cur >= 0 ? cur : 0; }
        else old = cur;
        cpu += now_ms() - c0;

        if (drmModePageFlip(fd, crtc, KB[b].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL)) {
            perror("PageFlip"); break;
        }
        pend = b; pend_old = old;
        wait_flip(fd);
        if (pend_old >= 0)
            __atomic_store_n(&c->ack[pend_old],
                             __atomic_load_n(&c->seq[pend_old], __ATOMIC_ACQUIRE),
                             __ATOMIC_RELEASE);
        cur = pend; pend = -1;
        shown++;

        double now = now_ms(), gap = now - last;
        if (gap > expected * 1.5) drops += (int)(gap / expected + 0.5) - 1;
        last = now;
    }
    double dt = now_ms() - t0;
    printf("%d 幀 / %.1f ms → %.1f fps（理論 %d Hz）\n", shown, dt, shown * 1000.0 / dt, mode.vrefresh);
    printf("合成器 CPU %.3f ms/幀 ｜ 漏拍 %d\n", cpu / (shown ? shown : 1), drops);

    close(cli); close(srv); unlink(sockpath);
    return 0;
}
