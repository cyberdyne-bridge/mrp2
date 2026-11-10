# mrp2-bench — MRP2 離屏驗證（階段 1）

驗證 MRP2 的協議、合成決策、光柵與輸入，**完全不碰 `/dev/dri` / DRM / KMS**，
所以不會影響目前 sway 桌面（第一工作區）。

## 一鍵跑

```bash
~/mrp2-bench/run.sh
```

會依序：

1. 檢查環境（CPU / kernel / Python / gcc / libdrm）
2. 編譯 `bench/raster.c`、`bench/plan.c`、`csrc/mrpd.c`、`kms/kms.c`（kms 只編譯不執行）
3. 跑 5 組測試（共 88 條斷言）
4. 光柵基準：C vs Python
5. 合成決策基準：200 surface / 50 damage / 500 幀
6. `mrpd` 渲染 1080p 文字頁 → PNG
7. `demo_proto.py` 全鏈路（client → 協議 → server → PNG）
8. `compositor.py --demo`（焦點 / layer / 節流 / 直掃）

## 產出

```
RESULTS-<timestamp>.md     摘要表
out/run-<timestamp>.log    完整原始輸出
out/text.png               mrpd 渲染的 1080p 文字頁
out/demo/*.png             協議全鏈路輸出
out/comp/*.png             合成器 demo 輸出
bin/                       編譯產物
```

## 重要結論（2026-11-10 實測，i7-6700）

- C 光柵 solid **0.10 ms**，Python **9.09 ms** → 差距約 **91×**（不是交接件寫的 1400×，那是 iSH 模擬層的數字）
- C 合成決策 **0.022 ms/幀**，Python **3.5 ms/幀**
- 60 Hz 預算 16.67 ms：Python 路線勉強可行
- 120 Hz 預算 8.33 ms：**必須走 C 核心**（Python 光是 plan 就吃掉 42%）

## 階段 2（需要 sudo，尚未執行）

真實 KMS 輸出不能在 sway 裡做，只能：

```bash
# Ctrl+Alt+F2 切到 tty2（sway 釋放 DRM master）
sudo ~/mrp2-bench/bin/kms /dev/dri/card2
```

先確認 NVIDIA 的 `CREATE_DUMB` 是否可用；若 `ENOSYS`，改走 GBM（見 `kms/README.md`）。

更理想的長期做法：把第二塊螢幕接到主機板 HDMI（Intel `card1`，目前所有接口都是 disconnected），
MRP2 獨占 iGPU，桌面繼續用 NVIDIA `card2`，可邊用邊測。

## 階段 2 實測（2026-11-10，已執行）

- `chvt 2` / `chvt 1` 正常 → **VT 切換本身沒問題**
- KMS 在 NVIDIA `card2` 上跑通：`CREATE_DUMB` 成功，**沒有**遇到 ENOSYS
- 選到模式 `1920x1080@120`，雙緩衝 page flip 正常
- 靜態 buffer（不重繪）：120 幀 / 995 ms = **120.6 fps**（跑滿 120 Hz）
- 原始 `kms.c`（每幀重繪）：120 幀 / 2031 ms = **59.1 fps**
  → `draw()` 約 16.9 ms/幀，是瓶頸，不是顯示鏈路
- 測完自動切回 tty1，sway 恢復、輸出回到 119.88 Hz

腳本：`kms-test.sh`（原版）；`bin/kms_nodraw`（測 flip 上限）

## 階段 3（渲染優化，2026-11-10）

做法：背景只算一次（`fill_bg`），每幀 `memcpy` 背景 + 只畫移動豎條（`draw_bar`）。

| 版本 | 幀率 | 每幀合成 |
|---|---|---|
| 原始 `kms.c`（逐像素重繪） | 59.1 fps | ~16.9 ms |
| **`kms_fast.c`（背景預計算 + memcpy）** | **120.0 fps** | **2.002 ms** |

- 120Hz 預算 8.33 ms，目前只吃掉 24%，還有 6 ms 餘裕
- 8 MB/幀 memcpy ≈ 960 MB/s，記憶體頻寬完全夠
- 下一步若要再壓：用 DAMAGE 只重畫舊/新豎條兩條窄帶（約 0.2 ms），或改三緩衝 + fence

檔案：`src/kms_fast.c` → `bin/kms_fast`
執行：`sudo env KMS_BIN=$HOME/mrp2-bench/bin/kms_fast ~/mrp2-bench/kms-test.sh /dev/dri/card2 120`

## 階段 4（DAMAGE 窄帶重繪，2026-11-10）

只把「舊豎條 ∪ 新豎條」覆蓋的列從背景還原，再畫新豎條；畫面其餘部分從不動。

| 版本 | 每幀合成 | 120Hz 下 fps |
|---|---|---|
| 原始逐像素 `kms.c` | ~16.9 ms | 59.1 |
| 整屏 memcpy `kms_fast.c` | 2.8 ms | 120.0 |
| **DAMAGE 窄帶 `kms_damage.c`** | **0.171 ms** | **120.0** |

- 0.171 ms 只占 120Hz 預算（8.33 ms）的 **2%**
- 每幀只搬 ~360 KB（舊+新豎條兩條窄帶），不是 8 MB
- 這正好對應 MRP2 協定的 `DAMAGE` 模型：只重畫真正變化的區域

檔案：`src/kms_damage.c` → `bin/kms_damage`

## 階段 5（三緩衝 + FLIP_COMPLETE 回收，2026-11-10）

3 個 dumb buffer，狀態機：

- `current`：正在掃描輸出
- `pending`：已提交 page flip、等 `FLIP_COMPLETE`
- 其餘：可寫

每幀流程：找一張 free buffer → DAMAGE 窄帶更新 → 等上一個 `FLIP_COMPLETE`（同時回收它取代掉的 buffer）→ 提交新 flip。

| 測試 | 結果 |
|---|---|
| 5 秒（600 幀） | 120.0 fps｜合成 0.210 ms｜漏拍 **0** |
| 15 秒（1800 幀） | 119.9 fps｜合成 0.191 ms｜漏拍 **0** |

- 三緩衝讓 render 與 vblank 等待重疊，不再卡在雙緩衝的「render 完才等」
- 每張 buffer 間隔 3 幀才重用，DAMAGE 帶稍寬（~18px），合成仍在 0.2 ms

檔案：`src/kms_triple.c` → `bin/kms_triple`

## 階段 6（協議 → KMS 整合，2026-11-10）

`src/mrpd_kms.c` = 原 `mrpd` 的解碼／plan／raster + KMS 三緩衝輸出：

- FB 是 XBGR8888，用 `drmModeAddFB2(DRM_FORMAT_XBGR8888)` 直接宣告，同步時**零轉換**
- 每個緩衝各自累積「上次同步以來的協議髒區」，重用時只同步這些矩形
- page flip 當節拍器，render 與 vblank 重疊

順手修了一個真 bug：`raster()` 的透明文字層原本**不理髒區**，每幀把整頁文字重畫。
加上 `CLIP` 裁剪 + 整行/整字跳過後：

| 模式 | CPU/幀 | 幀率 | 漏拍 |
|---|---|---|---|
| 協議 DAMAGE 同步（髒區） | **0.252 ms**（同步 0.130） | 119.9 fps | 0 |
| `--full` 每幀整屏同步 | 6.089 ms（同步 6.009） | 117.4 fps | 0 |

→ 髒區化讓每幀 CPU 少 **24 倍**，120Hz 預算（8.33 ms）只用 3%。

執行：

```bash
sudo sh -c 'chvt 2; /home/user17/mrp2-bench/bin/mrpd_kms \
  /home/user17/mrp2/assets/text.wire 600 120 \
  --font /home/user17/mrp2/assets/font.rom --card /dev/dri/card2; chvt 1'
```

## 階段 7（PLANE 共享記憶體影片，2026-11-10）

`plane.wire` 是一個 **1920×1080 全屏 PLANE**（XBGR8888，stride 7680）。`mrpd_kms --plane` mmap
共享記憶體，每幀把整塊影片區當作協議髒區：PLANE → FB → KMS。

### 除錯過程

| 版本 | 幀率 | CPU/幀 |
|---|---|---|
| 初版（每緩衝累積 3 幀整屏髒區 = 3×8 MB） | 97.1 fps | 9.86 ms |
| + 全寬單次 memcpy | 99.3 fps | 9.34 ms |
| + 矩形包含去重（新整屏幀取代舊的） | **119.9 fps** | **4.74 ms** |

### 穩定度（60 Hz 生產端持續更新共享記憶體）

| 時長 | 幀率 | CPU/幀 | 漏拍 |
|---|---|---|---|
| 5 秒（600 幀） | 119.9 fps | 4.66 ms（同步 2.46） | **0** |
| 15 秒（1800 幀） | 119.9 fps | 5.04 ms（同步 2.78） | **0** |

成本拆解：PLANE→FB 約 2.3 ms ＋ FB→KMS 約 2.5 ms。

**結論**：1080p 全屏影片走合成在 120 Hz 可行（約 56% 預算、零漏拍）；
但每次都要搬兩遍 8 MB。真正的「直掃」讓生產端直接寫入掃描緩衝，可省掉這兩次拷貝。

檔案：`bin/mrpd_kms`（--plane）、`src/plane_producer.c` → `bin/plane_producer`

## 階段 8（零拷貝直掃，2026-11-10）

| 方案 | 每幀合成 CPU | 幀率 | 備註 |
|---|---|---|---|
| 合成（PLANE→FB→KMS，兩次拷貝） | 4.74 ms | 119.9 | 階段 7 |
| 單次拷貝直掃（PLANE→KMS） | 2.55 ms | 120.0 | 跳過 FB |
| **零拷貝（GBM dmabuf 直寫）** | **0.000 ms** | **120.0** | 生產端直接寫掃描緩衝 |

關鍵發現：

- NVIDIA 專有驅動的 **PRIME 匯出 dumb buffer 只允許唯讀 mmap**（RW 回 EACCES），跨行程寫不了
- ��是 **GBM（`nvidia-drm_gbm.so`）配置的緩衝可以 RW mmap**，dmabuf fd 也能用 `SCM_RIGHTS` 傳給生產端
- 架構：伺服器建 3 個 GBM 掃描緩衝 → 傳 fd 給生產端 → 生產端直接寫 → 伺服器只 page flip
- 用 `seq/ack` 每個緩衝做 3 深度的生產者/消費者握手，避免寫到正在掃描的緩衝

檔案：`src/shmscan_server.c`、`src/shmscan_producer.c` → `bin/shmscan_*`

## 階段 9（NV12 / 硬體 YUV 直掃，2026-11-10）— 影片最優解

| 路徑 | 每幀成本 | 幀率 | 漏拍 |
|---|---|---|---|
| 合成（PLANE→FB→KMS，RGB） | 4.74 ms | 119.9 | 0 |
| 單次拷貝直掃（RGB） | 2.55 ms | 120.0 | 0 |
| 零拷貝 RGB（GBM dmabuf） | 合成器 0.000 ms | 120.0 | 0 |
| **NV12 主平面 page flip** | **填充 1.07 ms** | **120.1** | **0** |

- GBM NV12 緩衝：2 個 plane，stride 2048，UV offset 2228224，modifier=LINEAR
- `gbm_bo_map` 對 NV12 回 ENOSYS，但 **dmabuf fd 可以 RW mmap**
- `drmModeAddFB2(DRM_FORMAT_NV12)` + **主平面 page flip** 直接掃出，硬體 CSC
- 每幀只寫 3.1 MB（Y+UV）vs RGB 的 8 MB；120Hz 下約 373 MB/s
- overlay plane（`drmModeSetPlane`）也驗證可用，適合「影片在下、UI 在上」的合成

**注意**：想讓 BT.709 limited 顏色準確，要設 plane 的 `COLOR_ENCODING=BT709`、
`COLOR_RANGE=Limited`（`nv12_test.c` 有示範）。

檔案：`src/nv12_test.c`（overlay）、`src/nv12_flip.c`（primary flip）→ `bin/`

## 階段 10（真內容：ffmpeg 解碼 → NV12 → 直掃，2026-11-10）

```
ffmpeg -re -i video.mp4 -vf scale=1920:1080 -pix_fmt nv12 -f rawvideo - \
  | nv12_play /dev/dri/card2 120
```

- 讀執行緒從 stdin 讀 NV12 幀（3.1 MB/幀），做 stride 轉換後填進空閒的 GBM 掃描緩衝
- 主執行緒 120 Hz page flip；沒有新幀時重翻當前緩衝（30 fps 內容 → 每幀顯示 4 次）
- 3 緩衝 + `seq/ack` 握手，不會寫到正在掃描的緩衝

| 測試（1080p30 H.264，15 秒） | 顯示 | 影片幀 | 主迴圈 CPU | 漏拍 |
|---|---|---|---|---|
| 第一次 | 1773 幀 / 14.78 s = 119.9 fps | 450 | 0.000 ms/幀 | 1（開頭） |
| 第二次 | 1771 幀 / 14.77 s = 119.9 fps | 450 | 0.000 ms/幀 | **0** |

檔案：`src/nv12_play.c` → `bin/nv12_play`

## 階段 11（A/V + UI 疊加 + B 站，2026-11-10）

- 音訊：ffmpeg 單一行程，video→stdout（NV12），audio→ALSA `plughw:1,0`
- UI：overlay plane 進度條；**SetPlane 只在初始化做一次**，之後直接改映射記憶體
  （每次 SetPlane 會同步等 vblank，實測造成 451 次漏拍；改成一次後 0 漏拍）
- B 站：`yt-dlp` 解析直鏈（可解），但 mcdn 串流在 tty2 上會無限卡住 →
  改為**有界下載到本地再播放**

| 測試 | 顯示 | 影片幀 | CPU | 漏拍 |
|---|---|---|---|---|
| 本地 A/V 15 秒 | 1860 幀 / 15.51 s = 119.9 fps | 450 | 0.000 ms | 0 |
| B 站 12 秒（本地檔） | 1500 幀 / 12.51 s = 119.9 fps | 300 | 0.035 ms | 0 |

---

## ⚠️ 安全操作規則（2026-11-10 事故後）

**事故**：B 站串流在 tty2 卡住 → 用 `kill -9` 殺了持有 DRM master 的播放器 →
CRTC 留在它的 buffer 上、sway 搶不回輸出 → 只能重啟。

**規則**：

1. **任何碰 `/dev/dri` 的程式一律用 `run-safe.sh` 執行**，不要直接 sudo 跑
2. **網路/解碼先有界下載到本地**，不在 tty 上跑無限串流
3. **絕不 SIGKILL 持有 DRM master 的行程**；先 `chvt 1` 釋放 master，再殺
4. 每次結束**驗證 sway 輸出** `active=true` 且 119.88 Hz

```bash
sudo ~/mrp2-bench/run-safe.sh 40 <命令...>   # 硬逾時 + 行程組終止 + 保證恢復
~/mrp2-bench/play-bili.sh <URL> [秒數]        # 下載(有界) → 安全播放
```

`run-safe.sh` 實作：`setsid` 行程組、看門狗逾時、SIGTERM 優先、
必要時先 `chvt 1` 再 SIGKILL、結束後輪詢驗證 sway 恢復（失敗會 reload 並告警）。

### 音訊輸出（2026-11-10 修正）

- 實際喇叭接在 **USB 音效卡**：`plughw:0,0`（GeneralPlus USB Audio）
- 主板類比（`plughw:1,0`）雖然能開，但沒有接喇叭 → 先前「影片沒聲音」的原因
- HDMI（`plughw:2,*`）顯示器也支援 LPCM 2ch，但實測以 USB 為準
- 預設已改為 `MRP_AUDIO=plughw:0,0`；可用環境變數覆蓋

**注意**：bare ALSA 沒有混音伺服器，USB 音效卡是**獨佔**的 ——
Firefox／其他播放器正在用時，我們的播放器會 `Device or resource busy`。
要同時出聲需要 dmix 或 PipeWire，目前未配置。

## 階段 12（桌面場景渲染 + mrpd 修 bug，2026-11-10）

場景：頂部面板 + 兩個視窗 + 透明文字層（`assets/desktop.wire`）。

修正了三個真 bug：

1. **TEXT 透明標記**：只有「TEXT 自動建立 surface」時才設 `transparent=1`；
   客戶端先宣告 `SURF` 再送 `TEXT`（正常用法）時文字層會被當成不透明視窗填掉
2. **繪製順序**：`plan()` 由上往下產生 ops，`raster()` 卻照原序畫 → 底層蓋掉上層
3. **`drawn[]` 去重**：透明層只畫第一次，之後每個髒區的背景 op 會把文字蓋掉

字模：原 `font.rom` 只有 746 字形（文章用字）。改用 **Unifont 16px** 烘了 667 字形
（`assets/font16.rom`）；順手修了 `mkfont.py` 的 `data_off = 12 + index`（應為 **14**+index，
頭是 14 位元組）——這個 off-by-2 會讓字形資料整體錯位。

tty2 實測（1800 幀 / 15 秒）：**119.9 fps、0 漏拍、CPU 0.571 ms/幀**（同步 0.292）。

視覺評估：16px 點陣字清晰；視窗是平面淺色矩形 + 22px 標題列，無邊框/陰影/圓角；
版面靠寫死座標。協定本身沒有邊框、陰影、圓角這些裝飾屬性。

### 深色配色（2026-11-10）

| 元素 | 顏色 |
|---|---|
| 桌布 | `#0B0C0E` |
| 頂部面板 | `#14161A` |
| 視窗 | `#1A1D21` |
| 標題列 | `#2A2F36` |
| 正文 | `#C8CDD4` |
| 連結 | `#58A6FF` |
| 強調（播放狀態） | `#FFB86B` |
| 次要 | `#8B9099` / `#A8ADB5` / `#E6E9EE` |

tty2 實測：119.9 fps、0 漏拍、CPU 0.580 ms/幀。

## 阶段 13（tty2 桌面复刻 + 脏区更新 / 静止定格，2026-11-10）

### 场景

`assets/tty2desktop.wire`（8 surface / 41 段文字，1717 B）复刻 tty1 桌面：

| 元素 | 位置／尺寸 |
|---|---|
| 桌布 | 全屏 `#0B0C0E` |
| 顶栏 | 0,0 1920×34（MPR2·tty2 / CPU / MEM / DISK / 时间） |
| SYSTEM·SPEC 面板 | 40,64 430×330（HOST/OS/KERNEL/CPU/GPU/MEM/DISK/UPTIME） |
| dock | 40,980 600×76（FIREFOX…WIFI） |
| 终端窗口 | 520,70 780×560（fastfetch 输出） |
| 浏览器窗口 | 520,660 780×360（网易云音乐） |
| QQ 窗口 | 1340,70 540×900（会话列表 + 群公告） |

配色沿用深色盘但强调色用蓝 `#58A6FF` / 橙 `#FFB86B`，与 tty1 的绿色强调区分。

字模改用 GNU Unifont `.hex` 直接烘焙（`src/bake_hex.py`，不依赖 freetype）：
**7923 字形 / 329.5 KB**，覆盖 ASCII + 完整 GB2312 + 标点箭头。

### 游标与静止判定

- 首帧 raster 后把整张 FB 存成 `CLEAN`；之后**不重跑 plan/raster**
- 每次移动：还原旧、新两个 `14×20` 小矩形（从 `CLEAN`），重画游标，
  只把这两个矩形加入三缓冲各自的脏区并同步
- 3 秒无任何变化 → **完全停止 page flip**，画面定格；有新变化立刻恢复
- `--mouse` 自动扫描 `/dev/input/event*`，找有 `REL_X`/`REL_Y` 的设备
  （本机为 Logitech G402 → `event3`），真实鼠标驱动
- 无鼠标／`--mouse` 缺省时用脚本游标自动示范

### 实测（tty2，120 Hz）

脚本模式 23 秒：**1440 次更新，活动 120.0 Hz，每次工作 0.0170 ms，
每次脏区仅全屏 0.0270%**；3.0 秒静止时两次准确触发 `[idle]` 停止 flip，
第二次活动恢复后再次正常定格。

鼠标模式实测 /dev/input/event3 正常收到移动事件（6 秒 469 次更新，
0.0245 ms/次），松手后不再产生事件 → 可正常进入 3 秒定格。

### 启动

```bash
sudo ~/mrp2-bench/tty2-desktop.sh mouse 120     # 真实鼠标，2 分钟
sudo ~/mrp2-bench/tty2-desktop.sh scripted 23   # 自动示范
```

脚本内部一律走 `run-safe.sh`，结束后自动 `chvt 1` 并验证 sway 输出。
