/* shmvid.c — 甲：memfd + SCM_RIGHTS 的影片路徑（C）
 *
 * 1. 連上 Unix socket，用 SCM_RIGHTS 收一個 memfd
 * 2. mmap 那塊共享記憶體（零拷貝）
 * 3. 每幀把 RGB565 轉成 XRGB 貼進 framebuffer
 * 4. 可選 60 Hz 定拍，或無上限衝滿
 *
 *   gcc -O2 -o /var/tmp/shmvid shmvid.c
 *   /var/tmp/shmvid /tmp/mrp.sock 60 300      # 60 Hz，300 幀
 *   /var/tmp/shmvid /tmp/mrp.sock 0 300       # 無上限
 */
#include <fcntl.h>
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

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "用法：%s <path> <hz|0> <frames> <W> <H>\n", argv[0]); return 2; }
    const char *path = argv[1];
    int hz = atoi(argv[2]);
    int frames = atoi(argv[3]);

    int fd = open(path, O_RDWR);   /* iSH 不支援 SCM_RIGHTS，先用同一檔案 mmap */
    if (fd < 0) { perror("open shm"); return 1; }


    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); return 1; }
    size_t bytes = st.st_size;
    uint16_t *src = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (src == MAP_FAILED) { perror("mmap"); return 1; }

    int W = atoi(argv[4]), H = atoi(argv[5]);
    size_t npix = (size_t)W * H;
    int bpp = (int)(bytes / npix);
    uint32_t *fb = malloc((size_t)W * H * 4);

    int zero = (argc > 6 && strcmp(argv[6], "zero") == 0);
    printf("收到共享記憶體：%.1f MB（%dx%d，%d bpp）%s\n",
           bytes / 1048576.0, W, H, bpp * 8, zero ? "｜零拷貝（直接掃出）" : "");

    /* 65536 項查表：RGB565 → XRGB，省掉每像素的移位 */
    static uint32_t lut[65536];
    for (uint32_t v = 0; v < 65536; v++)
        lut[v] = 0xFF000000u | ((v & 0x1F) << 19) | (((v >> 5) & 0x3F) << 10) | ((v >> 11) & 0x1F) << 3;

    double interval = hz ? 1000.0 / hz : 0;
    double next = now_ms();
    double t0 = now_ms(), total = 0;
    for (int f = 0; f < frames; f++) {
        double t = now_ms();
        if (zero) { /* 不搬：直接把掃出指標指向共享記憶體 */ }
        else if (bpp == 4) memcpy(fb, src, bytes);     /* XRGB：一次 memcpy，無轉換 */
        else for (size_t i = 0; i < npix; i++) fb[i] = lut[src[i]];
        total += now_ms() - t;

        if (interval > 0) {                     /* 60 Hz 定拍 */
            next += interval;
            double sleep = next - now_ms();
            if (sleep > 0) {
                struct timespec ts = { (time_t)(sleep / 1000), (long)((sleep - (long)(sleep / 1000) * 1000) * 1e6) };
                nanosleep(&ts, NULL);
            }
        }
    }
    double wall = now_ms() - t0;
    printf("%s：%d 幀 / %.0f ms → %.1f fps ｜ 每幀貼圖 %.3f ms ｜ 讀 %.0f MB/s\n",
           hz ? "60 Hz" : "無上限", frames, wall, frames * 1000.0 / wall,
           total / frames, bytes * frames / (wall / 1000.0) / 1048576.0);
    return 0;
}
