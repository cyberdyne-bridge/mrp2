# kms — 最小 DRM/KMS 輸出端

不經 X11、不經 Wayland，直接拿 `/dev/dri/card0`：dumb buffer → `SetCrtc` → `PageFlip`。

```sh
gcc -O2 -o kms kms.c -I/usr/include/libdrm -ldrm
sudo ./kms /dev/dri/card0        # 要先離開圖形介面（拿 DRM master）
```

iSH 上已驗證**可編譯可連結**（libdrm 24.2.8），但沒有 `/dev/dri`，**沒跑過**。

## 為什麼先 CPU

| 階段 | 量級（已測） |
|---|---|
| 協議解析 | < 0.1 ms |
| 合成決策 | 0.24 ms |
| CPU 光柵（全屏 1920×1080） | 0.23 ms（C）／87 ms（Python） |

一幀 8.29 MB，60 fps 要 497 MB/s——記憶體完全吃得下。
60 Hz 預算是 16.67 ms：C 路線不到 1 ms，Python 全屏重繪會爆（87 ms），
所以伺服器核心用 C，Python 留在協議與工具層。

## HDMI 接在 N 卡上的注意事項

1. 內核參數要 `nvidia_drm.modeset=1`，否則 `/dev/dri/cardN` 不會給 KMS。
2. 專有驅動的 `CREATE_DUMB` 可能回 `ENOSYS`（nvidia 對 dumb buffer 支援一向曖昧）。
   程式已經把錯誤印出來；真遇到就改走 **GBM**：
   `gbm_create_device(fd)` → `gbm_bo_create(...)` → `drmModeAddFB2(...)`，
   或直接 EGL（`eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_dev, ...)`）。
3. 多卡時要挑「HDMI 那張」：`/dev/dri/card0` 不一定是 N 卡，先看
   `ls /sys/class/drm/card*/device/driver` 與 `cat /sys/class/drm/card*-HDMI-A-1/status`。
4. page flip 完成事件（`DRM_EVENT_FLIP_COMPLETE`）才是真正的 60 Hz 節拍；
   不要在 `draw` 之後 `usleep(16ms)`，那會累積漂移。
5. 執行時如果 TTY 上還有其他 display server，`drmSetMaster` 會失敗 → 先 `systemctl stop` 掉
   （或直接從 TTY 起）。

## 驗收命令

```sh
ls /sys/class/drm/                     # 看有哪些 connector、誰是 connected
cat /sys/class/drm/card0-HDMI-A-1/status
sudo ./kms                            # 應該看到彩條跑 120 幀，印出實測 fps
```
