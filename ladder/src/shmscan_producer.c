/* shmscan_producer — 零拷貝影片直掃（生產端）
 *
 * 連上 shmscan_server，用 SCM_RIGHTS 收 3 個掃描緩衝 fd + 1 個控制 memfd，
 * 直接把畫面寫進掃描緩衝（零拷貝），以固定 Hz 更新。
 *
 * 用法：shmscan_producer [hz] [socket]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define NBUF 3
#define W 1920
#define H 1080
#define BW 120

typedef struct {
    volatile uint32_t seq[NBUF];
    volatile uint32_t ack[NBUF];
} Ctrl;

typedef struct { uint32_t w, h, pitch, nbuf; } Hdr;

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static inline uint32_t px_rgb(int r, int g, int b) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}

static void fill_bg(uint32_t *p, int pitch_px) {
    for (int y = 0; y < H; y++) {
        uint32_t c = px_rgb(20 + 200 * y / H, 30 + 180 * y / H, 120);
        uint32_t *row = p + (size_t)y * pitch_px;
        for (int x = 0; x < W; x++) row[x] = c;
    }
}

static void fill_bar(uint32_t *p, int pitch_px, int x0, int x1, int solid_or_bg) {
    if (x0 < 0) x0 = 0;
    if (x1 > W) x1 = W;
    if (x1 <= x0) return;
    int y0 = H / 3, y1 = H * 2 / 3;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = p + (size_t)y * pitch_px;
        uint32_t c = solid_or_bg ? px_rgb(240, 160, 32)
                                 : px_rgb(20 + 200 * y / H, 30 + 180 * y / H, 120);
        for (int x = x0; x < x1; x++) row[x] = c;
    }
}

int main(int argc, char **argv) {
    int hz = argc > 1 ? atoi(argv[1]) : 60;
    const char *sockpath = argc > 2 ? argv[2] : "/tmp/mrp-scan.sock";

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, sockpath, sizeof(sa.sun_path) - 1);
    for (int i = 0; i < 100; i++) {
        if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0) break;
        usleep(100 * 1000);
        if (i == 99) { perror("connect"); return 1; }
    }

    Hdr h;
    struct iovec iov = {&h, sizeof(h)};
    char cbuf[CMSG_SPACE(sizeof(int) * 4)];
    struct msghdr msg = {0};
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);
    if (recvmsg(sock, &msg, 0) < 0) { perror("recvmsg"); return 1; }

    int fds[4];
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    if (!cm || cm->cmsg_type != SCM_RIGHTS) { fprintf(stderr, "沒收到 fd\n"); return 1; }
    memcpy(fds, CMSG_DATA(cm), sizeof(int) * 4);

    Ctrl *c = mmap(NULL, sizeof(Ctrl), PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
    if (c == MAP_FAILED) { perror("mmap ctrl"); return 1; }

    uint32_t *buf[NBUF];
    int pitch_px = h.pitch / 4;
    for (int i = 0; i < NBUF; i++) {
        buf[i] = mmap(NULL, (size_t)h.pitch * h.h, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fds[i + 1], 0);
        if (buf[i] == MAP_FAILED) { perror("mmap buf"); return 1; }
        fill_bg(buf[i], pitch_px);
    }
    printf("收到 %d 個掃描緩衝：%ux%u pitch=%u（零拷貝直寫）\n", NBUF, h.w, h.h, h.pitch);
    fflush(stdout);

    int last_bar[NBUF] = {0, 0, 0};
    int next = 0;
    double pace = now_ms();
    int frame = 0;

    for (;;) {
        int i = next;
        next = (next + 1) % NBUF;

        /* 等這個緩衝空閒（ack == seq）；順便偵測伺服器是否已結束 */
        for (;;) {
            uint32_t s = __atomic_load_n(&c->seq[i], __ATOMIC_ACQUIRE);
            uint32_t a = __atomic_load_n(&c->ack[i], __ATOMIC_ACQUIRE);
            if (s == a) break;
            char tmp;
            ssize_t r = recv(sock, &tmp, 1, MSG_DONTWAIT);
            if (r == 0) { printf("伺服器已結束\n"); return 0; }
            usleep(1000);
        }

        int nb = (frame * 12) % (W + 200) - 100;
        fill_bar(buf[i], pitch_px, last_bar[i], last_bar[i] + BW, 0);  /* 抹舊 */
        fill_bar(buf[i], pitch_px, nb, nb + BW, 1);                    /* 畫新 */
        last_bar[i] = nb;

        __atomic_store_n(&c->seq[i],
                         __atomic_load_n(&c->seq[i], __ATOMIC_ACQUIRE) + 1,
                         __ATOMIC_RELEASE);
        frame++;

        pace += 1000.0 / hz;
        double sl = pace - now_ms();
        if (sl > 0) {
            struct timespec ts = {(time_t)(sl / 1000),
                                  (long)((sl - (long)(sl / 1000) * 1000) * 1e6)};
            nanosleep(&ts, NULL);
        } else pace = now_ms();
    }
    return 0;
}
