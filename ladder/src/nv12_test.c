/* nv12_test — 最小 NV12 overlay plane 驗證
 *
 * 1. primary plane：黑底 XRGB
 * 2. GBM 配置 NV12 緩衝，填 SMPTE 色條（BT.709 limited）
 * 3. 找一個支援 NV12 的 overlay plane，drmModeSetPlane 掃出
 * 4. 顯示 4 秒後還原
 *
 * 用法：sudo nv12_test [card] [hz]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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

/* BT.709 limited range */
static void rgb2yuv(int r, int g, int b, int *Y, int *U, int *V) {
    *Y = 16 + (int)(0.1826 * r + 0.6142 * g + 0.0620 * b);
    *U = 128 + (int)(-0.1006 * r - 0.3386 * g + 0.4392 * b);
    *V = 128 + (int)(0.4392 * r - 0.3989 * g - 0.0403 * b);
}

int main(int argc, char **argv) {
    const char *card = argc > 1 ? argv[1] : "/dev/dri/card2";
    int hz = argc > 2 ? atoi(argv[2]) : 120;

    int fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }
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
    printf("mode=%dx%d@%d crtc=%u\n", mode.hdisplay, mode.vdisplay, mode.vrefresh, crtc);
    if (enc) drmModeFreeEncoder(enc);

    struct gbm_device *g = gbm_create_device(fd);
    if (!g) { perror("gbm_create_device"); return 1; }

    /* ── 1. primary：黑底 XRGB ── */
    struct gbm_bo *pbo = gbm_bo_create(g, W, H, GBM_FORMAT_XBGR8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE);
    if (!pbo) { perror("gbm_bo_create primary"); return 1; }
    uint32_t pstride = gbm_bo_get_stride(pbo);
    void *pmap = NULL;
    uint32_t pmap_stride = 0;
    void *pm = gbm_bo_map(pbo, 0, 0, W, H, GBM_BO_TRANSFER_WRITE, &pmap_stride, &pmap);
    if (!pm) { perror("gbm_bo_map primary"); return 1; }
    memset(pm, 0, (size_t)pmap_stride * H);
    gbm_bo_unmap(pbo, pm);
    uint32_t ph = gbm_bo_get_handle(pbo).u32;
    uint32_t ph4[4] = {ph,0,0,0}, pp4[4] = {pstride,0,0,0}, po4[4] = {0,0,0,0};
    uint32_t pfb;
    if (drmModeAddFB2(fd, W, H, DRM_FORMAT_XBGR8888, ph4, pp4, po4, &pfb, 0)) {
        perror("AddFB2 primary"); return 1;
    }
    if (drmModeSetCrtc(fd, crtc, pfb, 0, 0, &conn->connector_id, 1, &mode)) {
        perror("SetCrtc"); return 1;
    }

    /* ── 2. NV12 色條 ── */
    struct gbm_bo *vbo = gbm_bo_create(g, W, H, GBM_FORMAT_NV12,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR | GBM_BO_USE_WRITE);
    if (!vbo) { perror("gbm_bo_create NV12"); return 1; }
    printf("NV12 bo: planes=%d stride0=%u off0=%u stride1=%u off1=%u\n",
           gbm_bo_get_plane_count(vbo),
           gbm_bo_get_stride_for_plane(vbo, 0), gbm_bo_get_offset(vbo, 0),
           gbm_bo_get_stride_for_plane(vbo, 1), gbm_bo_get_offset(vbo, 1));

    static const int bars[8][3] = {
        {235,235,235},{235,235,0},{0,235,235},{0,235,0},
        {235,0,235},{235,0,0},{0,0,235},{0,0,0}
    };
    /* gbm_bo_map 對 NV12 回 ENOSYS，改直接 mmap dmabuf（modifier=LINEAR） */
    int vfd = gbm_bo_get_fd(vbo);
    uint32_t ys = gbm_bo_get_stride_for_plane(vbo, 0), yo = gbm_bo_get_offset(vbo, 0);
    uint32_t cs = gbm_bo_get_stride_for_plane(vbo, 1), co = gbm_bo_get_offset(vbo, 1);
    size_t vsz = (size_t)co + (size_t)cs * (H / 2);
    uint8_t *base = mmap(NULL, vsz, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, 0);
    if (base == MAP_FAILED) { perror("mmap NV12 dmabuf"); return 1; }
    uint8_t *Yp = base + yo, *UVp = base + co;

    for (int x = 0; x < W; x++) {
        int bi = x * 8 / W; if (bi > 7) bi = 7;
        int Y, U, V;
        rgb2yuv(bars[bi][0], bars[bi][1], bars[bi][2], &Y, &U, &V);
        for (int y = 0; y < H; y++) Yp[(size_t)y * ys + x] = (uint8_t)Y;
        for (int y = 0; y < H / 2; y++) {
            UVp[(size_t)y * cs + (x & ~1)] = (uint8_t)U;
            UVp[(size_t)y * cs + (x | 1)] = (uint8_t)V;
        }
    }

    uint32_t vh = gbm_bo_get_handle(vbo).u32;
    uint32_t vh4[4] = {vh, vh, 0, 0};
    uint32_t vp4[4] = {ys, cs, 0, 0};
    uint32_t vo4[4] = {yo, co, 0, 0};
    uint32_t vfb;
    if (drmModeAddFB2(fd, W, H, DRM_FORMAT_NV12, vh4, vp4, vo4, &vfb, 0)) {
        perror("AddFB2 NV12"); return 1;
    }
    printf("AddFB2 NV12 OK, fb=%u\n", vfb);

    /* ── 3. 找 NV12 overlay plane ── */
    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    int done = 0;
    for (uint32_t i = 0; i < pr->count_planes && !done; i++) {
        drmModePlane *p = drmModeGetPlane(fd, pr->planes[i]);
        if (!p) continue;
        int has_nv12 = 0;
        for (uint32_t k = 0; k < p->count_formats; k++)
            if (p->formats[k] == DRM_FORMAT_NV12) has_nv12 = 1;
        if (!has_nv12) { drmModeFreePlane(p); continue; }

        /* 跳過 primary（SetCrtc 後 crtc_id 會指向它） */
        if (p->crtc_id == crtc) {
            printf("plane %u 是 primary，跳過\n", p->plane_id);
            drmModeFreePlane(p); continue;
        }

        int rc = drmModeSetPlane(fd, p->plane_id, crtc, vfb, 0,
                                 0, 0, W, H,
                                 0, 0, W << 16, H << 16);
        printf("SetPlane %u: %s\n", p->plane_id, rc ? strerror(errno) : "OK");
        if (!rc) {
            /* 設定 BT.709 limited（若有這兩個 property） */
            drmModeObjectProperties *props = drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
            if (props) {
                for (uint32_t k = 0; k < props->count_props; k++) {
                    drmModePropertyRes *prp = drmModeGetProperty(fd, props->props[k]);
                    if (!prp) continue;
                    if (!strcmp(prp->name, "COLOR_ENCODING")) {
                        for (int e = 0; e < prp->count_enums; e++)
                            if (!strcmp(prp->enums[e].name, "BT709"))
                                drmModeObjectSetProperty(fd, p->plane_id, DRM_MODE_OBJECT_PLANE,
                                                         prp->prop_id, prp->enums[e].value);
                    } else if (!strcmp(prp->name, "COLOR_RANGE")) {
                        for (int e = 0; e < prp->count_enums; e++)
                            if (!strcmp(prp->enums[e].name, "Limited"))
                                drmModeObjectSetProperty(fd, p->plane_id, DRM_MODE_OBJECT_PLANE,
                                                         prp->prop_id, prp->enums[e].value);
                    }
                    drmModeFreeProperty(prp);
                }
                drmModeFreeObjectProperties(props);
            }
            printf("顯示 4 秒（NV12 八色條）...\n");
            fflush(stdout);
            sleep(4);
            drmModeSetPlane(fd, p->plane_id, crtc, 0, 0, 0,0,0,0, 0,0,0,0);
            done = 1;
        }
        drmModeFreePlane(p);
    }
    if (!done) printf("沒有成功啟用 NV12 overlay plane\n");
    drmModeFreePlaneResources(pr);
    return done ? 0 : 1;
}
