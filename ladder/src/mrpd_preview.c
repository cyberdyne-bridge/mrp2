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
    for (int d = 0; d < ND; d++) fill(DMG[d], 0x00101010);        /* 桌布底色 */
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
        if (s->transparent) {                     /* 透明層：只畫字，不填底 */
            for (int ti = 0; ti < s->ntxt; ti++) {
                TextRun *t = &s->txt[ti];
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
                    if (gi >= 0) { draw_glyph(gi, cx, t->y, t->color); cx += GW[gi]; }
                    else cx += 8;
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

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "用法：mrpd scene.wire out.ppm [幀數] [寬 高]\n"); return 2; }
    W = argc > 4 ? atoi(argv[4]) : 1920;
    H = argc > 5 ? atoi(argv[5]) : 1080;
    int frames = argc > 3 ? atoi(argv[3]) : 1;
    int hz = argc > 6 ? atoi(argv[6]) : 0;
    const char *plane_path = NULL, *font_path = NULL;
    for (int i = 6; i < argc; i++) {
        if (!strcmp(argv[i], "--font") && i + 1 < argc) font_path = argv[++i];
        else if (!strcmp(argv[i], "--plane") && i + 1 < argc) plane_path = argv[++i];
        else if (!strcmp(argv[i], "--prewarm")) OPT_PREWARM = 1;
        else if (!strcmp(argv[i], "--rows")) OPT_ROWS = 1;
        else if (!strcmp(argv[i], "--budget")) OPT_BUDGET = 1;
    }
    if (font_path && !load_font(font_path)) return 1;
    if (font_path) printf("字模：%u 個字形\n", NGLYPH);
    FB = malloc((size_t)W * H * 4);

    if (plane_path) {
        int pfd = open(plane_path, O_RDONLY);
        if (pfd < 0) { perror("plane"); return 1; }
        struct stat st;
        fstat(pfd, &st);
        PS = (int)(st.st_size / H);
        PLANE = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, pfd, 0);
        if (PLANE == MAP_FAILED) { perror("mmap plane"); return 1; }
        printf("共享記憶體：%.1f MB（stride %d）\n", st.st_size / 1048576.0, PS);
        if (OPT_PREWARM) {
            volatile uint8_t sum = 0;
            for (size_t i = 0; i < (size_t)st.st_size; i += 4096) sum ^= ((uint8_t *)PLANE)[i];
            if (mlock(FB, (size_t)W * H * 4) != 0)
                madvise(FB, (size_t)W * H * 4, MADV_WILLNEED);
            printf("預熱：掃過 %.1f MB（mlock/madvise 已嘗試）\n", st.st_size / 1048576.0);
            (void)sum;
        }
    }

    double t_dec = 0;
    if (!load_wire(argv[1], &t_dec)) return 1;
    for (int i = 0; i < NS; i++) ORDER[i] = i;
    qsort(ORDER, NS, sizeof(int), cmp_rank);

    double t_plan = 0, t_rast = 0;
    plan(&t_plan); raster(&t_rast);
    if (argc > 2) save_ppm(argv[2]);

    int spans = 0; for (int i = 0; i < NS; i++) spans += S[i].nsp;
    printf("解碼 %.2f ms ｜ %d surface ／ %d span\n", t_dec, NS, spans);
    printf("首幀：plan %.2f ms ／ raster %.2f ms ／ ops %d\n", t_plan, t_rast, NO);

    /* 直掃判定：唯一可見且全屏、不透明的 plane surface → 不搬 */
    int scan = 0;
    for (int i = 0; i < NS; i++) {
        Surf *s = &S[i];
        if (!s->visible) continue;
        if (s->has_plane && s->x == 0 && s->y == 0 && s->w == W && s->h == H &&
            (s->flags & 1) && s == &S[ORDER[NS - 1]]) scan = 1;
    }
    if (scan) printf("直掃：合成器零拷貝，掃出指標直接指向共享記憶體\n");

    double *ft = calloc(frames > 1 ? frames : 1, sizeof(double));

    if (frames > 1) {
        /* 之後每幀只改一條文字帶（模擬打字），量決策＋光柵的極限 */
        double total = 0;
        int missed = 0;
        double start = now_ms(), next = start;
        for (int f = 0; f < frames; f++) {
            ND = 0; NO = 0;
            /* 模擬 30 fps 影片：每 2 幀更新一次，且只有下半屏變動 */
            for (int i = 0; i < NS; i++) if (S[i].has_plane && (OPT_BUDGET || f % 2 == 0)) {
                Rect r = {S[i].x, S[i].y + S[i].h / 2, S[i].w, S[i].h / 2};
                for (int yy = r.y; yy < r.y + r.h && yy < H; yy++)
                    if (S[i].rowdirty) S[i].rowdirty[yy] = 1;
                add_damage(r);
            }
            for (int i = 0; i < NS; i++)
                if (S[i].role != 0 && S[i].visible)
                    add_damage((Rect){S[i].x, S[i].y + 22 + (f % 20) * 16, S[i].w, 16});
            double p, r = 0;
            plan(&p);
            if (!scan) raster(&r);
            total += p + r;
            ft[f] = p + r;
            if (hz) {
                next += 1000.0 / hz;
                double sleep = next - now_ms();
                if (sleep <= 0) missed++;
                else {
                    struct timespec ts = {(time_t)(sleep / 1000), (long)((sleep - (long)(sleep / 1000) * 1000) * 1e6)};
                    nanosleep(&ts, NULL);
                }
            }
        }
        int cmp_d(const void *a, const void *b) {
            double x = *(const double *)a, y = *(const double *)b;
            return (x > y) - (x < y);
        }
        qsort(ft, frames, sizeof(double), cmp_d);
        double p50 = ft[frames / 2];
        double p99 = ft[(int)(frames * 0.99)];
        double pmax = ft[frames - 1];
        double wall = now_ms() - start;
        printf("%d 幀 / %.0f ms → %.1f fps ｜ CPU %.3f ms/幀%s\n",
               frames, wall, frames * 1000.0 / wall, total / frames,
               hz ? "（定拍）" : "（無上限）");
        printf("  幀時間 p50 %.3f ｜ p99 %.3f ｜ max %.3f ms ｜ 漏拍 %d 幀（%.1f%%）\n",
               p50, p99, pmax, missed, 100.0 * missed / frames);
    }
    return 0;
}
