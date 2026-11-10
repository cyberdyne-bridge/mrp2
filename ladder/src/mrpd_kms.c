/* mrpd.c — MRP2 的 C 版伺服器核心：解碼 → 合成決策 → 軟體光柵
 *
 * 吃 Python 端產生的 .wire（MRP2 二進位），輸出 PPM。
 * 目的：同一份協議、同一份畫面，換語言跑，量真實上限。
 *
 *   gcc -O2 -o /var/tmp/mrpd mrpd.c
 *   /var/tmp/mrpd out/doc.wire out/c.ppm 120
 */
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_S 64
#define MAX_SPANS 8192
#define MAX_DMG 256
#define MAX_PIECES 512
#define MAX_OPS 4096

typedef struct { int x, y, w, h; } Rect;
typedef struct { uint16_t y, x0, x1; uint8_t c; } Span;
typedef struct { int x, y; uint32_t color; char *s; } TextRun;

typedef struct {
    uint16_t id; int x, y, w, h, z, role, flags, visible;
    uint32_t color; Span *sp; int nsp, cap; int has_plane; uint8_t *rowdirty;
    int transparent; TextRun *txt; int ntxt, captxt;
} Surf;
typedef struct { int sid; Rect r; } Op;

static Surf S[MAX_S]; static int NS = 0;
static Rect DMG[MAX_DMG]; static int ND = 0;
static Op OPS[MAX_OPS]; static int NO = 0;
static uint32_t *FB; static int W = 0, H = 0;
static Rect CLIP = {0, 0, 1 << 30, 1 << 30};   /* 目前的繪製裁剪框（= op 的髒區） */
static uint32_t *PLANE = NULL; static int PS = 0;   /* 共享記憶體（XRGB，寬度同輸出） */
static int OPT_PREWARM = 0, OPT_ROWS = 0, OPT_BUDGET = 0;

/* ── 字模 ROM（MRPF） ── */
static uint8_t *FONT = NULL;
static uint32_t FONT_DATA = 0, NGLYPH = 0;
static uint32_t *GCP = NULL, *GOFF = NULL;
static uint8_t *GW = NULL, *GH = NULL, *GP = NULL;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static int load_font(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("font"); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    FONT = malloc(n); fread(FONT, 1, n, f); fclose(f);
    if (memcmp(FONT, "MRPF", 4)) { fprintf(stderr, "字模 magic 不對\n"); return 0; }
    NGLYPH = rd32(FONT + 6);
    FONT_DATA = rd32(FONT + 10);
    GCP = malloc(4 * NGLYPH); GOFF = malloc(4 * NGLYPH);
    GW = malloc(NGLYPH); GH = malloc(NGLYPH); GP = malloc(NGLYPH);
    for (uint32_t i = 0; i < NGLYPH; i++) {
        const uint8_t *e = FONT + 14 + i * 12;
        GCP[i] = rd32(e); GOFF[i] = rd32(e + 4);
        GW[i] = e[8]; GH[i] = e[9]; GP[i] = e[10];
    }
    return 1;
}

static int find_glyph(uint32_t cp) {
    int lo = 0, hi = (int)NGLYPH - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (GCP[mid] == cp) return mid;
        if (GCP[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
#define BUDGET_ROWS 256

static void fill(Rect r, uint32_t c);            /* 前置宣告 */
static void draw_glyph(int gi, int x, int y, uint32_t color);

static const uint32_t PAL[6] = {
    0x00D4CDC8,  /* #C8CDD4 正文 */
    0x00FFA658,  /* #58A6FF 連結 */
    0x006BB8FF,  /* #FFB86B 強調 */
    0x0099908B,  /* #8B9099 次要 */
    0x00B5ADA8,  /* #A8ADB5 次亮 */
    0x00EEE9E6   /* #E6E9EE 最亮 */
};

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int rank_of(Surf *s) { return s->role == 0 ? 0 : (s->role == 2 ? 2 : (s->role == 3 ? 3 : 1)); }

static int cmp_rank(const void *a, const void *b) {
    int i = *(const int *)a, j = *(const int *)b;
    int ri = rank_of(&S[i]), rj = rank_of(&S[j]);
    if (ri != rj) return ri - rj;
    if (S[i].z != S[j].z) return S[i].z - S[j].z;
    return S[i].id - S[j].id;
}
static int ORDER[MAX_S];

static Surf *find(uint16_t id) {
    for (int i = 0; i < NS; i++) if (S[i].id == id) return &S[i];
    return NULL;
}

static Surf *add_surf(uint16_t id, int x, int y, int w, int h, int z, int role, int flags) {
    Surf *s = &S[NS++];
    memset(s, 0, sizeof *s);
    s->id = id; s->x = x; s->y = y; s->w = w; s->h = h; s->z = z; s->role = role;
    s->flags = flags; s->visible = !(flags & 2);
    /* 深色模式：桌布 #0B0C0E、面板 #14161A、視窗 #1A1D21（FB 為 XBGR，值為 R|G<<8|B<<16） */
    s->color = (role == 0) ? 0x000E0C0B : (role == 2 ? 0x001A1614 : 0x00211D1A);
    s->cap = 1024;
    s->sp = malloc(sizeof(Span) * s->cap);
    s->rowdirty = calloc(H > 0 ? H : 1, 1);
    return s;
}

static void add_damage(Rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    if (ND < MAX_DMG) DMG[ND++] = r;
}

static void add_span(Surf *s, uint16_t y, uint16_t x0, uint16_t x1, uint8_t c) {
    if (s->nsp >= s->cap) {
        s->cap *= 2; s->sp = realloc(s->sp, sizeof(Span) * s->cap);
    }
    s->sp[s->nsp++] = (Span){y, x0, x1, c};
}

/* ── 解碼 ── */
static int load_wire(const char *path, double *ms) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("open wire"); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n); fread(b, 1, n, f); fclose(f);

    double t = now_ms();
    if (n < 11 || memcmp(b, "MRP2", 4) && memcmp(b, "MRP1", 4)) { fprintf(stderr, "magic 不對\n"); return 0; }
    uint32_t nops; memcpy(&nops, b + 7, 4);
    size_t off = 11;
    for (uint32_t k = 0; k < nops; k++) {
        uint8_t type = b[off]; uint16_t len; memcpy(&len, b + off + 1, 2); off += 3;
        uint8_t *p = b + off; off += len;
        if (type == 1) {                       /* SURF 17 B */
            uint16_t id; int16_t x, y, z; uint16_t w, h; uint8_t layer, flags, role; uint16_t parent;
            memcpy(&id, p, 2); memcpy(&x, p + 2, 2); memcpy(&y, p + 4, 2);
            memcpy(&w, p + 6, 2); memcpy(&h, p + 8, 2); memcpy(&z, p + 10, 2);
            layer = p[12]; flags = p[13]; role = p[14]; memcpy(&parent, p + 15, 2);
            (void)layer; (void)parent;
            Surf *s = find(id);
            if (!s) s = add_surf(id, x, y, w, h, z, role, flags);
            else { s->x = x; s->y = y; s->w = w; s->h = h; s->z = z; s->role = role;
                   s->visible = !(flags & 2); s->flags = flags; }
            add_damage((Rect){x, y, w, h});
        } else if (type == 3) {                /* SPANS */
            uint16_t id, tx, ty, cnt;
            memcpy(&id, p, 2); memcpy(&tx, p + 2, 2); memcpy(&ty, p + 4, 2); memcpy(&cnt, p + 6, 2);
            Surf *s = find(id); if (!s) continue;
            uint8_t *q = p + 8;
            for (int i = 0; i < cnt; i++, q += 7) {
                uint16_t yy, x0, x1;
                memcpy(&yy, q, 2); memcpy(&x0, q + 2, 2); memcpy(&x1, q + 4, 2);
                add_span(s, (uint16_t)(ty * 16 + yy), x0, x1, q[6]);
            }
            add_damage((Rect){s->x, s->y + 22 + ty * 16, s->w, 16});
        } else if (type == 4) {                /* COMMIT */
            uint16_t id; memcpy(&id, p, 2); Surf *s = find(id);
            if (s) add_damage((Rect){s->x, s->y, s->w, s->h});
        } else if (type == 5) {                /* UNMAP */
            uint16_t id; memcpy(&id, p, 2); Surf *s = find(id); if (s) s->visible = 0;
        } else if (type == 10) {               /* TEXT */
            uint16_t id, len; int16_t x, y; uint8_t color;
            memcpy(&id, p, 2); memcpy(&x, p + 2, 2); memcpy(&y, p + 4, 2);
            color = p[6]; memcpy(&len, p + 7, 2);
            Surf *ts = find(id);
            if (!ts) {
                ts = add_surf(id, 0, 0, W, H, 0, 1, 0);
                ts->color = 0;
            }
            ts->transparent = 1;   /* TEXT 層只畫墨；不論 SURF 是否先宣告 */
            if (ts->ntxt >= ts->captxt) {
                ts->captxt = ts->captxt ? ts->captxt * 2 : 32;
                ts->txt = realloc(ts->txt, sizeof(TextRun) * ts->captxt);
            }
            char *copy = malloc(len + 1);
            memcpy(copy, p + 9, len); copy[len] = 0;
            ts->txt[ts->ntxt++] = (TextRun){x, y, PAL[color % 6], copy};
            if (getenv("MRPD_DEBUG")) fprintf(stderr, "[text] sid=%u n=%d \"%s\"\n", id, ts->ntxt, copy);
            add_damage((Rect){x, y, W - x, 20});
        } else if (type == 9) {                /* PLANE（共享記憶體） */
            uint16_t id, w, h; int16_t x, y; uint16_t stride;
            memcpy(&id, p, 2); memcpy(&x, p + 2, 2); memcpy(&y, p + 4, 2);
            memcpy(&w, p + 6, 2); memcpy(&h, p + 8, 2); memcpy(&stride, p + 10, 2);
            Surf *s = find(id);
            if (!s) s = add_surf(id, x, y, w, h, 0, 1, 1);
            s->has_plane = 1;
            if (s->rowdirty)
                for (int yy = y; yy < y + h && yy < H; yy++) s->rowdirty[yy] = 1;
            add_damage((Rect){x, y, w, h});
            (void)stride;
        } else if (type == 2) {                /* DAMAGE */
            uint16_t id; int16_t x, y; uint16_t w, h;
            memcpy(&id, p, 2); memcpy(&x, p + 2, 2); memcpy(&y, p + 4, 2);
            memcpy(&w, p + 6, 2); memcpy(&h, p + 8, 2);
            Surf *s = find(id); if (s) add_damage((Rect){s->x + x, s->y + y, w, h});
        }
    }
    free(b);
    *ms = now_ms() - t;
    return 1;
}

/* ── 光柵小工具 ── */
static uint32_t *ROWBUF = NULL;
static int ROWCAP = 0;

static void fill(Rect r, uint32_t c) {
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > W) r.w = W - r.x;
    if (r.y + r.h > H) r.h = H - r.y;
    if (r.x < CLIP.x) { r.w += r.x - CLIP.x; r.x = CLIP.x; }
    if (r.y < CLIP.y) { r.h += r.y - CLIP.y; r.y = CLIP.y; }
    if (r.x + r.w > CLIP.x + CLIP.w) r.w = CLIP.x + CLIP.w - r.x;
    if (r.y + r.h > CLIP.y + CLIP.h) r.h = CLIP.y + CLIP.h - r.y;
    if (r.w <= 0 || r.h <= 0) return;
    if (r.w > ROWCAP) {
        ROWCAP = r.w * 2;
        ROWBUF = realloc(ROWBUF, sizeof(uint32_t) * ROWCAP);
    }
    for (int x = 0; x < r.w; x++) ROWBUF[x] = c;
    size_t bytes = (size_t)r.w * 4;
    for (int y = 0; y < r.h; y++)
        memcpy(FB + (size_t)(r.y + y) * W + r.x, ROWBUF, bytes);
}

static void draw_glyph(int gi, int x, int y, uint32_t color) {
    const uint8_t *bmp = FONT + FONT_DATA + GOFF[gi];
    int w = GW[gi], h = GH[gi], pitch = GP[gi];
    if (getenv("MRPD_DEBUG") && gi == 730)
        fprintf(stderr, "  [glyph] data=%u off=%u w=%d h=%d pitch=%d bytes=%02x %02x %02x %02x\n",
                FONT_DATA, GOFF[gi], w, h, pitch, bmp[0], bmp[1], bmp[2], bmp[3]);
    for (int ry = 0; ry < h; ry++) {
        const uint8_t *row = bmp + ry * pitch;
        int rx = 0;
        while (rx < w) {
            if (row[rx >> 3] & (0x80 >> (rx & 7))) {
                int s = rx;
                while (rx < w && (row[rx >> 3] & (0x80 >> (rx & 7)))) rx++;
                fill((Rect){x + s, y + ry, rx - s, 1}, color);
            } else rx++;
        }
    }
}

static Rect isect(Rect a, Rect b) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w < b.x + b.w) ? a.x + a.w : b.x + b.w;
    int y1 = (a.y + a.h < b.y + b.h) ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0) return (Rect){0, 0, 0, 0};
    return (Rect){x0, y0, x1 - x0, y1 - y0};
}
static int empty(Rect r) { return r.w <= 0 || r.h <= 0; }
static Rect uni(Rect a, Rect b) {
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = (a.x + a.w > b.x + b.w) ? a.x + a.w : b.x + b.w;
    int y1 = (a.y + a.h > b.y + b.h) ? a.y + a.h : b.y + b.h;
    return (Rect){x0, y0, x1 - x0, y1 - y0};
}

static int sub(Rect r, Rect cut, Rect *out) {
    Rect i = isect(r, cut);
    if (empty(i)) { out[0] = r; return 1; }
    int n = 0;
    if (i.y > r.y) out[n++] = (Rect){r.x, r.y, r.w, i.y - r.y};
    if (i.y + i.h < r.y + r.h) out[n++] = (Rect){r.x, i.y + i.h, r.w, r.y + r.h - (i.y + i.h)};
    if (i.x > r.x) out[n++] = (Rect){r.x, i.y, i.x - r.x, i.h};
    if (i.x + i.w < r.x + r.w) out[n++] = (Rect){i.x + i.w, i.y, r.x + r.w - (i.x + i.w), i.h};
    return n;
}

/* ── 合成決策 ── */
static void plan(double *ms) {
    double t = now_ms();
    NO = 0;
    for (int d = 0; d < ND; d++) {
        int start = NO;
        Rect pieces[MAX_PIECES]; int np = 1; pieces[0] = DMG[d];
        for (int k = NS - 1; k >= 0; k--) {
            Surf *s = &S[ORDER[k]];
            if (!s->visible) continue;
            Rect sr = (Rect){s->x, s->y, s->w, s->h};
            Rect nxt[MAX_PIECES]; int nn = 0;
            for (int q = 0; q < np; q++) {
                Rect it = isect(pieces[q], sr);
                if (empty(it)) { nxt[nn++] = pieces[q]; continue; }
                if (NO < MAX_OPS) OPS[NO++] = (Op){s->id, it};
                if (s->flags & 1) {                      /* opaque */
                    Rect parts[4]; int m = sub(pieces[q], it, parts);
                    for (int w = 0; w < m; w++) nxt[nn++] = parts[w];
                } else nxt[nn++] = pieces[q];
            }
            memcpy(pieces, nxt, sizeof(Rect) * nn); np = nn;
            if (!np) break;
        }
        /* 這組是「上→下」產生的；反轉成「下→上」，raster 才能照 painter 順序畫 */
        for (int a = start, b = NO - 1; a < b; a++, b--) {
            Op tmp = OPS[a]; OPS[a] = OPS[b]; OPS[b] = tmp;
        }
    }
    *ms = now_ms() - t;
}

static void raster(double *ms) {
    double t = now_ms();
    for (int d = 0; d < ND; d++) { CLIP = DMG[d]; fill(DMG[d], 0x00101010); }  /* 桌布底色 */
    if (getenv("MRPD_DEBUG")) {
        fprintf(stderr, "[plan] NO=%d ND=%d\n", NO, ND);
        for (int i = 0; i < NO && i < 6; i++)
            fprintf(stderr, "  op sid=%d rect=(%d,%d,%d,%d)\n",
                    OPS[i].sid, OPS[i].r.x, OPS[i].r.y, OPS[i].r.w, OPS[i].r.h);
    }
    /* ops 已由下往上排序，照順序畫（painter's algorithm） */
    for (int i = 0; i < NO; i++) {
        Surf *s = find(OPS[i].sid); if (!s) continue;
        Rect r = OPS[i].r;
        CLIP = r;
        if (s->transparent) {                     /* 透明層：只畫字，依 CLIP 裁到髒區 */
            for (int ti = 0; ti < s->ntxt; ti++) {
                TextRun *t = &s->txt[ti];
                if (t->y + 24 <= CLIP.y || t->y >= CLIP.y + CLIP.h) continue;  /* 整行不在髒區 */
                const char *q = t->s;
                int cx = t->x;
                while (*q) {
                    uint32_t cp = (uint8_t)*q++;
                    if (cp >= 0xC0) {
                        int extra = (cp >= 0xF0) ? 3 : (cp >= 0xE0 ? 2 : 1);
                        cp &= (0x3F >> extra);
                        while (extra-- > 0) cp = (cp << 6) | ((uint8_t)*q++ & 0x3F);
                    }
                    int gi = find_glyph(cp);
                    if (getenv("MRPD_DEBUG") && ti == 0)
                        fprintf(stderr, "  cp=%x gi=%d w=%d h=%d\n", cp, gi,
                                gi >= 0 ? GW[gi] : -1, gi >= 0 ? GH[gi] : -1);
                    if (gi >= 0) {
                        if (cx + GW[gi] > CLIP.x && cx < CLIP.x + CLIP.w)
                            draw_glyph(gi, cx, t->y, t->color);
                        cx += GW[gi];
                    } else cx += 8;
                }
            }
            continue;
        }
        fill(r, s->color);
        if (s->role != 0) {
            Rect bar = isect((Rect){s->x, s->y, s->w, 22}, r);
            if (!empty(bar)) fill(bar, 0x00362F2A);               /* 標題列 #2A2F36 */
        }
        if (s->has_plane && PLANE) {                 /* 一次 memcpy，無轉換 */
            if (!OPT_ROWS && !OPT_BUDGET && r.x == 0 && r.w == W && PS == W * 4) {
                memcpy(FB + (size_t)r.y * W, PLANE + (size_t)r.y * W,
                       (size_t)r.h * W * 4);
                continue;
            }
            int done = 0;
            for (int y = r.y; y < r.y + r.h; y++) {
                if (OPT_ROWS && s->rowdirty && !s->rowdirty[y]) continue;
                if (OPT_BUDGET && done >= BUDGET_ROWS) break;   /* 單幀預算 */
                memcpy(FB + (size_t)y * W + r.x, PLANE + (size_t)y * (PS / 4) + r.x,
                       (size_t)r.w * 4);
                if (OPT_ROWS && s->rowdirty) s->rowdirty[y] = 0;
                done++;
            }
            continue;
        }
        for (int j = 0; j < s->nsp; j++) {
            Span *sp = &s->sp[j];
            Rect seg = isect((Rect){s->x + sp->x0, s->y + 22 + sp->y,
                                    sp->x1 - sp->x0, 7}, r);
            if (!empty(seg)) fill(seg, PAL[sp->c % 6]);
        }
    }
    if (getenv("MRPD_DEBUG")) fprintf(stderr, "[after] FB0=%08x FB@60,60=%08x\n", FB[0], FB[60*W+60]);
    *ms = now_ms() - t;
}

static int save_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror("ppm"); return 0; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t p = FB[(size_t)y * W + x];
            uint8_t rgb[3] = {(uint8_t)(p & 0xFF), (uint8_t)((p >> 8) & 0xFF), (uint8_t)((p >> 16) & 0xFF)};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    return 1;
}

/* ════════════ KMS 輸出端（三緩衝 + 協議 DAMAGE 同步） ════════════
 *
 * 前段是原 csrc/mrpd.c（解碼／plan／raster）。
 * 這裡把 FB 接到 DRM/KMS：每個緩衝只同步「協議髒區」，然後 page flip。
 */
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>
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

static int rect_contains(Rect outer, Rect inner) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

static void bd_add(DmgList *d, Rect r) {
    if (empty(r)) return;
    /* 新矩形若包含已有的，直接取代（整屏新幀就是這種情況） */
    int keep = 0;
    for (int i = 0; i < d->n; i++)
        if (!rect_contains(r, d->r[i])) d->r[keep++] = d->r[i];
    d->n = keep;
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
        if (!SWAP_RB && x0 == 0 && x1 == W && (int)k->pitch == W * 4) {
            memcpy(k->map + (size_t)y0 * k->pitch,
                   (uint8_t *)FB + (size_t)y0 * W * 4,
                   (size_t)(y1 - y0) * W * 4);
            continue;
        }
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

/* ── 游標 + 靜止偵測 ─────────────────────────────────────────────
 * 桌布只 raster 一次，存進 CLEAN；之後每次只還原／重畫游標所在的
 * 兩個小矩形，並只把這些矩形同步到掃描緩衝。
 * 3 秒沒有任何變化 → 不再 page flip（畫面定格），有變化再恢復。
 */
static uint32_t *CLEAN = NULL;

#define CW 12
#define CH 18
static const char *CURSOR_BM[CH] = {
    "o...........",
    "oo..........",
    "o#o.........",
    "o##o........",
    "o###o.......",
    "o####o......",
    "o#####o.....",
    "o######o....",
    "o#######o...",
    "o########o..",
    "o#########o.",
    "o##########o",
    "o##o#o......",
    "o#o..o#o....",
    "oo....o#o...",
    ".......o#o..",
    "........o#o.",
    ".........oo.",
};

static void draw_cursor(int x, int y) {
    for (int r = 0; r < CH; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= H) continue;
        const char *row = CURSOR_BM[r];
        for (int c = 0; c < CW; c++) {
            char ch = row[c];
            if (ch == '.' || ch == 0) continue;
            int xx = x + c;
            if (xx < 0 || xx >= W) continue;
            FB[(size_t)yy * W + xx] = (ch == '#') ? 0x00EEE9E6u : 0x000E0C0Bu;
        }
    }
}

static Rect cursor_rect(int x, int y) {
    return (Rect){x - 1, y - 1, CW + 2, CH + 2};
}

static void restore_rect(Rect r) {
    int x0 = r.x < 0 ? 0 : r.x;
    int y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w; if (x1 > W) x1 = W;
    int y1 = r.y + r.h; if (y1 > H) y1 = H;
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; y++)
        memcpy(FB + (size_t)y * W + x0, CLEAN + (size_t)y * W + x0,
               (size_t)(x1 - x0) * 4);
}

/* 只同步指定小矩形，翻到空閒緩衝；回傳實際繪製＋同步工作時間 */
static double present_rects(Rect *rs, int n, int fd, uint32_t crtc) {
    double w0 = now_ms();
    int b = -1;
    for (int i = 0; i < NBUF; i++)
        if (i != current && i != pending) { b = i; break; }
    if (b < 0) b = (current + 1) % NBUF;
    for (int i = 0; i < NBUF; i++)
        for (int r = 0; r < n; r++) bd_add(&BD[i], rs[r]);
    double s = 0;
    sync_buf(b, &s);
    double work = now_ms() - w0;

    if (pending >= 0) {
        wait_flip(fd);
        current = pending;
        pending = -1;
    }
    if (drmModePageFlip(fd, crtc, KB[b].fb, DRM_MODE_PAGE_FLIP_EVENT, NULL))
        perror("PageFlip");
    else
        pending = b;
    return work;
}

/* 找第一個有 REL_X / REL_Y 的 evdev 裝置（通常是滑鼠） */
static int mouse_open_auto(void) {
    for (int i = 0; i < 32; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int mfd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (mfd < 0) continue;

        unsigned long evbit = 0, relbit = 0;
        if (ioctl(mfd, EVIOCGBIT(0, sizeof(evbit)), &evbit) < 0 ||
            !(evbit & (1ul << EV_REL)) ||
            ioctl(mfd, EVIOCGBIT(EV_REL, sizeof(relbit)), &relbit) < 0 ||
            !(relbit & (1ul << REL_X)) || !(relbit & (1ul << REL_Y))) {
            close(mfd);
            continue;
        }
        printf("滑鼠輸入：%s\n", path);
        return mfd;
    }
    fprintf(stderr, "找不到滑鼠，改用腳本游標\n");
    return -1;
}

static void cursor_demo(int fd, uint32_t crtc, int seconds, int mfd) {
    CLEAN = malloc((size_t)W * H * 4);
    if (!CLEAN) { fprintf(stderr, "malloc CLEAN 失敗\n"); return; }
    memcpy(CLEAN, FB, (size_t)W * H * 4);

    int scripted = (mfd < 0);
    int cx = 880, cy = 470, dx = 7, dy = 4;
    draw_cursor(cx, cy);
    Rect r0 = cursor_rect(cx, cy);
    present_rects(&r0, 1, fd, crtc);

    double t0 = now_ms(), last_move = t0;
    double work_total = 0;
    long long dmg_px = 0;
    int updates = 0, idles = 0, idle = 0;
    int accx = 0, accy = 0;

    printf("游標示範：%d 秒（%s；3 秒無變化定格）\n",
           seconds, scripted ? "腳本活動 0-6s、11-17s" : "真實滑鼠，可自由移動");
    for (;;) {
        double t = (now_ms() - t0) / 1000.0;
        if (t >= seconds) break;

        int moved = 0, stepx = 0, stepy = 0;

        if (scripted) {
            int active = (t < 6.0) || (t >= 11.0 && t < 17.0);
            if (active) {
                stepx = dx; stepy = dy;
                moved = 1;
            } else {
                usleep(idle ? 100000 : 8000);
            }
        } else {
            struct pollfd p = {mfd, POLLIN, 0};
            int pr = poll(&p, 1, idle ? 100 : 8);
            if (pr > 0 && (p.revents & POLLIN)) {
                struct input_event ev;
                while (read(mfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                    if (ev.type == EV_REL) {
                        if (ev.code == REL_X) accx += ev.value;
                        else if (ev.code == REL_Y) accy += ev.value;
                    } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                        if (accx || accy) { stepx += accx; stepy += accy; accx = accy = 0; moved = 1; }
                    }
                }
            }
        }

        if (moved) {
            if (idle) {
                printf("[resume] 偵測到變化，恢復渲染\n");
                idle = 0;
            }
            int ox = cx, oy = cy;
            if (scripted) {
                cx += stepx; cy += stepy;
                if (cx < 180 || cx > 1700) { dx = -dx; cx += 2 * dx; }
                if (cy < 180 || cy > 860)  { dy = -dy; cy += 2 * dy; }
            } else {
                cx += stepx; cy += stepy;
                if (cx < 0) cx = 0;
                if (cy < 0) cy = 0;
                if (cx > W - CW) cx = W - CW;
                if (cy > H - CH) cy = H - CH;
            }
            if (cx == ox && cy == oy) continue;

            double c0 = now_ms();
            Rect ro = cursor_rect(ox, oy), rn = cursor_rect(cx, cy);
            restore_rect(ro);
            restore_rect(rn);
            draw_cursor(cx, cy);
            double draw_ms = now_ms() - c0;
            Rect rs[2] = {ro, rn};
            double w = present_rects(rs, 2, fd, crtc);
            work_total += draw_ms + w;
            dmg_px += (long long)ro.w * ro.h + (long long)rn.w * rn.h;
            updates++;
            last_move = now_ms();
        } else {
            double quiet = now_ms() - last_move;
            if (!idle && quiet >= 3000.0) {
                idle = 1;
                idles++;
                printf("[idle] %.1f 秒無變化 → 停止 page flip，畫面定格（已更新 %d 次）\n",
                       quiet / 1000.0, updates);
            }
        }
    }
    if (pending >= 0) wait_flip(fd);

    double dt = now_ms() - t0;
    long long full_px = (long long)W * H * (updates ? updates : 1);
    printf("更新 %d 次 / %.1f 秒 ｜ 每次工作 %.4f ms ｜ 髒區平均 %.4f%% 全屏\n",
           updates, dt / 1000.0, work_total / (updates ? updates : 1),
           100.0 * dmg_px / full_px);
    printf("定格 %d 次（3 秒無變化即停 flip）\n", idles);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "用法：mrpd_kms scene.wire [幀數] [Hz] [--font path] [--plane path] [--card /dev/dri/cardN] [--full]\n");
        return 2;
    }
    const char *wire = argv[1];
    int frames = argc > 2 ? atoi(argv[2]) : 600;
    int hz = argc > 3 ? atoi(argv[3]) : 120;
    const char *font_path = NULL;
    const char *plane_path = NULL;
    const char *card = "/dev/dri/card2";
    int cursor_mode = 0, seconds = 23, want_mouse = 0;
    const char *mouse_path = NULL;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
        else if (!strcmp(argv[i], "--plane") && i + 1 < argc) plane_path = argv[++i];
        else if (!strcmp(argv[i], "--full")) OPT_FULLSYNC = 1;
        else if (!strcmp(argv[i], "--cursor")) cursor_mode = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mouse")) {
            want_mouse = 1;
            if (i + 1 < argc && argv[i + 1][0] == '/') mouse_path = argv[++i];
        }
    }
    if (frames <= 0) frames = 600;

    W = 1920;
    H = 1080;
    if (font_path && !load_font(font_path)) return 1;
    FB = malloc((size_t)W * H * 4);
    if (!FB) { fprintf(stderr, "malloc FB 失敗\n"); return 1; }
    if (plane_path) {
        int pfd = open(plane_path, O_RDONLY);
        if (pfd < 0) { perror("plane"); return 1; }
        struct stat st;
        if (fstat(pfd, &st) != 0) { perror("fstat plane"); return 1; }
        PS = (int)(st.st_size / H);
        PLANE = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, pfd, 0);
        if (PLANE == MAP_FAILED) { perror("mmap plane"); return 1; }
        close(pfd);
        printf("PLANE：%.1f MB（stride %d）\n", st.st_size / 1048576.0, PS);
    }

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

    if (cursor_mode) {
        int mfd = -1;
        if (want_mouse) {
            if (mouse_path) {
                mfd = open(mouse_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (mfd < 0) perror(mouse_path);
                else printf("滑鼠輸入：%s\n", mouse_path);
            } else {
                mfd = mouse_open_auto();
            }
        }
        cursor_demo(fd, crtc, seconds > 0 ? seconds : 23, mfd);
        if (mfd >= 0) close(mfd);
        return 0;
    }

    /* ── 主迴圈：內容更新 → plan/raster → 髒區同步 → page flip ── */
    double t0 = now_ms();
    double last_complete = t0;
    double expected = 1000.0 / (mode.vrefresh ? mode.vrefresh : 60);
    double cpu_total = 0, sync_total = 0;
    int drops = 0;

    int scan_mode = 0;
    for (int i = 0; i < NS; i++) {
        Surf *s = &S[i];
        if (!s->visible) continue;
        if (s->has_plane && s->x == 0 && s->y == 0 && s->w == W && s->h == H &&
            (s->flags & 1) && s == &S[ORDER[NS - 1]]) scan_mode = 1;
    }
    if (scan_mode)
        printf("直掃模式：全屏 PLANE 直接進掃描緩衝（跳過 FB 合成）\n");

    for (int f = 1; f <= frames; f++) {
        double c0 = now_ms();

        int b = -1;
        for (int i = 0; i < NBUF; i++)
            if (i != current && i != pending) { b = i; break; }
        if (b < 0) b = (current + 1) % NBUF;

        if (scan_mode) {
            /* 單次拷貝：PLANE → 掃描緩衝（省掉 PLANE→FB→buffer 的中轉） */
            double s0 = now_ms();
            Kbuf *k = &KB[b];
            if (PLANE && (int)k->pitch == W * 4 && PS == W * 4) {
                memcpy(k->map, PLANE, (size_t)W * H * 4);
            } else if (PLANE) {
                for (int y = 0; y < H; y++)
                    memcpy(k->map + (size_t)y * k->pitch,
                           (uint8_t *)PLANE + (size_t)y * PS, (size_t)W * 4);
            }
            sync_total += now_ms() - s0;
        } else {
            /* 一般合成：協議髒區 → plan/raster → 髒區同步 */
            ND = 0;
            NO = 0;
            for (int i = 0; i < NS; i++) {
                if (!S[i].visible) continue;
                if (S[i].has_plane) {
                    add_damage((Rect){S[i].x, S[i].y, S[i].w, S[i].h});
                } else if (S[i].role != 0) {
                    add_damage((Rect){S[i].x, S[i].y + 22 + (f % 20) * 16, S[i].w, 16});
                }
            }
            plan(&t_plan);
            raster(&t_rast);

            for (int i = 0; i < NBUF; i++) {
                if (OPT_FULLSYNC) {
                    bd_add(&BD[i], (Rect){0, 0, W, H});
                } else {
                    for (int d = 0; d < ND; d++) bd_add(&BD[i], DMG[d]);
                }
            }

            double s = 0;
            sync_buf(b, &s);
            sync_total += s;
        }

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
