# minireader — 自研渲染協議 MRP2（PoC）

> **交接件：[HANDOVER.md](HANDOVER.md)**（新工作區開工用；協議權威文件是 [PROTOCOL.md](PROTOCOL.md)）


一句話：**把「瀏覽器」拆成四段，只做中間兩段**，純標準庫、零依賴。
文字頁像看論壇（列表 → 內文），媒體只給「連結＋簡介」，播放交給 mpv 小窗。

```
fetch ──▶ filter ──▶ layout ──▶ render
(urllib) (html.parser) (自寫)    (ANSI)
              │
              └─▶ media card ──▶ mpv（外部進程，不自己解碼）
                                   └─ JSON IPC：loadfile / seek / pause
```

## 檔案

| 檔 | 行數級 | 作用 |
|---|---|---|
| `minireader.py` | ~400 | 四段核心：抓頁／過濾／排版／渲染 |
| `reader.py` | ~160 | 論壇式前端：列表、跟隨、媒體小窗 |
| `tui.py` | ~230 | curses TUI：真鍵位，核心邏輯可無終端自測 |
| `resolver.py` | ~170 | 包 yt-dlp：頁面 URL → 可播直鏈 + 簡介卡 |
| `PROTOCOL.md` | — | **MRP1 渲染協議 v1 的權威文件**（位元組佈局、範例、v2 待辦） |
| `proto.py` | ~130 | 協議編解碼（binary） |
| `server.py` | ~110 | 吃協議 → 合成 → 翻頁 |
| `display.py` | ~70 | 顯示後端：Mock（可跑）／DRM（骨架） |
| `evdev.py` | ~180 | /dev/input → MRP2 事件（鍵盤／滑鼠／修飾鍵） |
| `demo_video.py` | ~90 | BUFFER 路徑（自生成 24 幀假影片） |
| `demo_video_real.py` | ~90 | ffmpeg 解碼 → BUFFER → PNG |
| `csrc/shmvid.c` | ~120 | 共享記憶體影片路徑（RGB565→XRGB 查表） |
| `demo_shm.py` | ~80 | 生產端：填 1080p 幀 → 交給 C |
| `kms/kms.c` | ~150 | DRM/KMS 最小輸出端（dumb buffer + page flip） |
| `render.py` | ~180 | Renderer 介面 + 軟體光柵 → PNG（iSH 可跑） |
| `bili.py` | ~70 | B 站連結正規化（BV／av／iframe／短鏈） |
| `test_resolver.py` | ~100 | 注入假 runner，14 項斷言 |
| `test_bili.py` | ~50 | 11 種 B 站寫法 + 反例 |
| `sample.html` | 2 KB | 模擬真實部落格（nav／文章／清單／表格／影音／pre） |
| `minified.html` | 0.5 KB | 壓成一行的站，用來打對照組 |
| `torture.html` | 1 KB | 邊界：超長網址、禁則標點、三層清單、殘缺表格 |
| `naive.py` | 12 | 對照組：正則剝標籤 |

## v0.2：論壇式導航 + 媒體小窗

```sh
python3 reader.py https://www.gnu.org/ --list      # 論壇列表視圖
python3 reader.py https://www.gnu.org/ --open 8    # 跟第 8 條
python3 reader.py sample.html                      # 內文 + 媒體卡
python3 reader.py sample.html --play 1 --dry       # 只印 mpv 命令
```

列表長這樣（標題對齊、URL 去 scheme、長 query 收成 `?…`）：

```
─ The GNU Operating System and the Free Software Movement ────────
  1. Skip to main text               www.gnu.org/#…
  8. ABOUT GNU                       www.gnu.org/gnu/gnu.html
```

媒體卡長這樣（不做播放器，只當附件）：

```
┌ 媒體 #1 · video ──────────────────────────
│ https://cdn.example.com/a.mp4
│ ↳ 小窗播放：v1
└───────────────────────────────────────────
```

播放指令（由 reader 生成，沙盒裡沒有 mpv 所以只印不跑）：

```
mpv --no-border --geometry=360x203 --input-ipc-server=/tmp/mini.mpv.sock -- '<url>'
for_window [app_id="mpv"] floating enable, resize set 360 203, move position center
```

**嵌入方式三選一**：

1. **外部視窗 + compositor 規則**（Wayland/sway 唯一可行）：mpv 自己開 client 窗，靠 `for_window` 讓它浮動、定位、不搶焦點。
2. **`--wid` 嵌進自家視窗**：只有 X11 有效，Wayland 沒有 window id 這回事。
3. **`--vo=tct`／`--vo=kitty`**：終端內直接出圖，代價是無聲、無字幕體驗差。

**控制通道**：`--input-ipc-server` 開 JSON socket，reader 只發 `loadfile`／`seek`／`pause`，解碼與渲染留在 mpv 進程。崩了也只崩播放器。

## v0.3：TUI 鍵位（甲）

```sh
python3 tui.py sample.html              # 真 curses
python3 tui.py sample.html --selftest   # 不開終端，驗 reducer
```

鍵位：`j/k` 或 ↑/↓ 移動，空格／`J` 翻頁，`g/G` 頭尾，`⏎`／`o` 開連結，
`v` 播媒體（往上找 6 行，落在媒體卡附近就能按），`l` 列連結，`b`／`Esc` 返回，`q` 離開。

**設計**：`act(session, key) → action` 是純函數（`'redraw'`／`'back'`／`('open', url)`／`('play', n)`），
畫面外殼只有 `draw()` 幾十行。所以鍵位邏輯不需要終端就能測；另外已用 pty 實跑過真畫面。

## v0.4：媒體解析（乙）

```sh
python3 resolver.py https://youtu.be/xxxx     # 簡介卡
python3 test_resolver.py                      # 11 項斷言（沙盒無 yt-dlp 也能驗）
```

```
┌ 影片 · 03:42 · 720p ──────────────────────────
│ 測試影片：把瀏覽器拆成三段
│ 來源：魚書館
│ 直鏈：cdn.example.com（mp4）
│ ↳ 播放：mpv --geometry=360x203 -- '<直鏈>'
└──────────────────────────────────────────────
```

規則：
- **直鏈**（`.mp4`／`.mp3` 等）→ 直接給 mpv，不多花一次解析。
- **需要解析**（`blob:`、`.m3u8`／`.mpd`、無副檔名頁面）→ 走 `yt-dlp -j`。
- 格式挑選：先排除無聲軌（`acodec=none`），再在 ≤720p 取最高，避免 4K 解不動又白耗流量。
- 結果快取 `/tmp/minireader_resolve.json`，TTL 一小時；解析器掛了也還能重播。
- 沒有 yt-dlp 時**降級不報錯**：回 None，上層退回 `mpv --ytdl`。

## B 站適配（實測，不是推測）

三層接線，缺一層就播不動：

1. **連結正規化**（`bili.py`）：BV 號／av 號／`m.bilibili.com`／`player.bilibili.com` iframe／`b23.tv` 短鏈，全部收斂成
   `https://www.bilibili.com/video/BV...?p=N`。過濾器遇到 iframe 就先做這一步，媒體卡的 tag 會顯示 `bilibili`。
2. **解析**（`resolver.py`）：交 `yt-dlp -j`；B 站匿名只給 DASH 分軌，所以同時拿影像軌與音軌。
3. **播放參數**：必須帶 `--referrer='https://www.bilibili.com/'` 與瀏覽器 UA，否則 CDN 直接 403；
   分軌時用 `--audio-file='<音軌>'` 把聲音掛第二軌，**不需要 ffmpeg 合流**。

實測（BV1GJ411x7h7）：

```
┌ 影片 · 03:32 · 1080p ─────────────────────────────────────
│ 【官方 MV】Never Gonna Give You Up - Rick Astley
│ 來源：索尼音乐中国
│ 直鏈：…mcdn.bilivideo.cn:8082（mp4，分軌）
└───────────────────────────────────────────────────────────
影像軌：HTTP 206　音軌：HTTP 206（帶 Referer 才通）
```

**順手驗到的兩件事**（也是「為什麼一定要 resolver」的鐵證）：
- 抓下 B 站頁面 161 KB：`<video>` 標籤 **0 個**，`<script>` 26 個。
- `window.__playinfo__` 已經不內嵌了（現在是簽名 API），所以「純文字站模式 + 抓 src」對 B 站完全無效。

裝法（Artix）：`pacman -S mpv yt-dlp`，可選 `ffmpeg`（要合流或轉檔才需要）。

## v0.5：渲染器（iSH 上能做的）

```sh
python3 minireader.py sample.html --emit --width 72 > out/doc.json
python3 render.py out/doc.json -o out/preview.png --scale 2
```

```
Renderer 介面：begin / rect / line / text / flush
├─ SoftRenderer  → 直接寫 RGB 緩衝，zlib 手搓 PNG（零依賴，已跑）
└─ GLRenderer    → 殼，等 EGL（quad batch + MSDF 字型）
```

**這是版面檢視器，不是排版成品**：沒有字型檔，CJK 用方塊表示，框線字元畫成真線。
它的價值是驗證幾何與 dirty-rect，不是給人讀。

實測（iSH，616×744 與 1232×1488）：

| 動作 | 耗時 |
|---|---|
| 全畫（scale 1） | 113.8 ms |
| 只重畫 6 行脏區 | 25.9 ms（省 77%） |
| 寫 PNG（zlib） | 339 ms |

**結論**：`damage` 省下的 77% 不是因為 shader 裡跳過像素，而是**根本沒去畫那幾行**——
正好對應前面那句「跳過要發生在幾何層，不是像素層」。

### iSH 上做不了、要等 Artix 的
- EGL／GLES（沒有 GPU 直通）→ GLRenderer 只能是殼
- 真字型（沙盒裡沒有任何 TTF）→ MSDF／FreeType
- mpv（播放）、Wayland／xdg-shell（開窗）、fcitx5（輸入法）

## v1.2：點陣字＋TEXT op（C 端已接）

```sh
python3 mkfont.py out/font.rom          # Unifont → MRPF 字模
mrpd scene.wire out.ppm 1 1920 1080 --font out/font.rom
```

- `TEXT` op：`id, x, y, color, len, utf8`——合成器自己查字模、畫點陣，客戶端不傳像素。
- 字模 ROM（MRPF）：746 字形 **30.4 KB**；換算全套：ASCII 1.5 KB、GB2312 193 KB、
  CJK 基本區 656 KB、整個 BMP 2.0 MB。
- 透明文字層：`transparent` surface 只畫字、不填底，跟桌布層分離（就是圖層分離的實作）。
- 1920×1080 一頁：33 條 TEXT、2 335 B 協議、587 個字形、首幀光柵 8.85 ms（含全屏鋪底）。

## v1.1：影片走共享記憶體（甲）

```sh
gcc -O2 -o /var/tmp/shmvid csrc/shmvid.c
python3 demo_shm.py 60 180     # 60 Hz 定拍
python3 demo_shm.py 0 200      # 無上限
```

三條路都量了（1920×1080，iSH）：

| 路徑 | 每幀 | 上限 |
|---|---|---|
| RGB565 → XRGB（查表） | 15.6 ms | 64 fps |
| **XRGB → memcpy** | **1.68 ms** | 594 fps |
| **XRGB → 零拷貝（直接掃出）** | **0 ms** | 由 vblank 決定 |

60 Hz 定拍：180 幀 / 3003 ms → 59.9 fps。記憶體 7.9 MB/幀，雙緩衝 15.8 MB。

**1% Low（600 幀，視窗化 1080p，30 fps 影片只動下半屏）**

| 設定 | p99 | max | 漏拍 |
|---|---|---|---|
| 基準 | 3.86 ms | 14.72 ms | 1 幀（0.2%） |
| `--prewarm`（掃過頁面＋mlock） | 3.76 ms | 6.99 ms | 0 |
| `--rows`（只複製髒列） | 4.06 ms | **4.51 ms** | 0 |
| 兩者一起 | 4.27 ms | 4.70 ms | 0 |
| `--budget`（256 列／幀） | 4.97 ms | 13.73 ms | 0 |

直掃：p99 0.165 ms、max 1.15 ms、**零漏拍**。

`--budget` 把單幀工作量切成 256 列，漏拍歸零，但代價是畫面落後（256 列×60 = 15 360 列/s，
全屏 1080 列要 4 幀才補完）——適合 UI 突發，不適合影片。

**還沒做**：三緩衝＋fence。iSH 沒有真的 page flip 事件可等，只能先量「漏拍」當代理指標；
到 Artix 用 `drmModePageFlip` + `DRM_EVENT_FLIP_COMPLETE` + 三個 dumb buffer 才是真的。

**已接進 C 伺服器**（`PLANE` op）：合成器每幀只做兩件事之一——

| 情況 | CPU/幀 | 幀率 |
|---|---|---|
| 全屏直掃（唯一不透明 surface） | **0.025 ms** | 60.0 fps |
| 視窗化（960×540 memcpy） | 1.4–6.1 ms | 60.0 / 698 fps（無上限） |

iSH 沒有 `memfd_create` 也不支援 `SCM_RIGHTS`（EINVAL），所以這裡用「同一檔案 mmap」
驗證零拷貝路徑；到 Artix 換回 memfd + fd 傳遞即可。

## v1.0：輸入層（evdev）

```sh
python3 test_evdev.py             # 18 項
python3 evdev.py --dump /dev/input/event3
python3 evdev.py --seat /dev/input/event3 /dev/input/event4
```

- 直接讀 `struct input_event`（24 bytes），不經 libinput。
- `EV_REL` 累積成絕對座標並夾在輸出邊界內；`EV_KEY`（BTN_LEFT／RIGHT／MIDDLE）→ `POINTER`。
- 鍵碼 → keysym：ASCII 直接用、特殊鍵用 X11 keysym；`Shift/Control/Alt` 累加成 `mods`。
- 點擊 = 改焦點；放開不改；未知鍵靜默忽略（不炸）。
- 事件一律帶 `serial`，客戶端可對齊自己的狀態機。

## v0.9：自家協議 v2 ＋ 顯示後端（拋棄 X11／Wayland）

```
自家 App ──MRP1 binary──▶ Server ──Compositor.plan──▶ Display.flip()
                                     │
                                     └─ spans（只有墨）／damage／scanout
```

系統級設計（herole／事件／幀同步／與 X11・Wayland 對照）：**[PROTOCOL.md](PROTOCOL.md)**。

v2 新增：`HELLO` 握手與能力協商、`role`（background/toplevel/panel/overlay/popup）、
server→client 事件（`KEY`/`POINTER`/`FOCUS`/`FRAME`/`CONFIGURE`/`ERROR`）、
`FRAME` 幀回呼；v1 封包仍可解。測試 17/17。

協議只傳「有內容的水平 span」，**黑底不上線**：

```
SURF   id x y w h z layer flags
SPANS  id tilex tiley n × (y x0 x1 color)
COMMIT id        ← 只代表這批送完，不膨脹髒區
```

實測（960×600，44 行文件 → 315 個 span）：

| 項目 | 數字 |
|---|---|
| 首幀封包 | 2 524 bytes |
| 同內容的 RGBA 幀 | 2 304 000 bytes（**913×**） |
| 第二幀（改一行） | **34 bytes**，合成 8 K 像素 |

顯示後端兩張臉：`MockDisplay`（記憶體＋翻頁，iSH 可跑）、`DRMDisplay`（真 `/dev/dri`，留骨架——
沒有 `libdrm` 與 KMS 之前不假裝能跑）。

```sh
python3 demo_proto.py out      # 全鏈路：client → 協議 → server → PNG
python3 test_proto.py          # 14 項斷言
```

## v0.8：把命令送到 GPU（iSH 已驗證）

GPU 不會「知道」你要什麼，它只執行你提交的 command buffer。要它幹活，得用 API 把工作描述出來：

| 工作 | API | 我們的用途 |
|---|---|---|
| 畫（逐像素） | GLES／Vulkan draw call | 每個 surface 一個 quad、文字 atlas |
| 算（平行） | compute shader／CUDA | MSDF 生成、影像處理 |
| 解（影片） | NVDEC | `mpv --hwdec=nvdec` |

`gpu/glclear.c`＋`gpu/glquad.c`：手寫 EGL/GLES ABI 宣告（不需要標頭），
在 iSH 上用 Mesa llvmpipe（純軟體）跑通整條鏈路：

```
EGL 1.5 surfaceless → GLES 3.2 (Mesa 24.2.8) → FBO → 1 次 draw call
→ glReadPixels → PPM → PNG（320×180，2.2 s）
```

也就是說：程式碼路徑是對的，到了 Artix 上同一份程式會打到 2060。
（有趣的是：複雜 shader 在 llvmpipe 上 JIT 要 3 分鐘以上，簡單 shader 2.2 秒——
真顯卡上是毫秒級，這個差距純粹是「模擬環境」的稅。）

**N 卡專屬（Artix）**
- 專有驅動要 `nvidia_drm.modeset=1`，EGL 走 GBM platform，不是 X11。
- wlroots 系偶爾要 `WLR_NO_HARDWARE_CURSORS=1`；顯式同步（explicit sync）跟驅動版本綁。
- 想全開源可以試 NVK（Mesa 的 Vulkan 驅動，Turing 支援），但那是 Vulkan 路線。
- 影片永遠交給 NVDEC，不要自己寫 shader 解碼。

## v0.7：合成器五件套

```sh
python3 compositor.py --demo out   # 焦點／layer／內容／節流／直掃 一次跑完
python3 test_compositor.py         # 28 項斷言
```

1. **輸入焦點**：`hit()` 由上往下命中；點擊改焦點；`focus_cycle()` 走 MRU（alt-tab）；
   鍵盤事件只送焦點視窗。面板（`keyboard=False`）點它不搶焦點。
2. **layer-shell**：`background < normal < top < overlay`；背景層 `input=False` 不參與命中；
   有 `top` 面板時不直掃（實測退回一般合成）。
3. **display list 進 surface**：`Surface.content` 直接吃 `--emit` 的 lines，視窗內可滾動、可裁剪。
4. **幀節流**：60 Hz 格線 + 對齊式累加（不漂移）；200 ms 內 101 次 damage → **13 幀**。
5. **C 決策核心**：`bench/plan.c`，同規模場景（200 surface × 50 damage × 500 幀）
   **0.244 ms/幀**，Python 版 **271 ms/幀**（約 1100×）。

| 決策 | 對應真實協議 |
|---|---|
| 髒區合併／裁剪 | `wl_surface.damage` |
| 上層不透明不合成下層 | `set_opaque_region` |
| 單一全屏不透明 → 直接掃出 | direct scanout |
| 焦點／alt-tab | `wl_seat` + 客戶端自己的 focus stack |
| 面板／桌布層 | `wlr-layer-shell` |

渲染後端（`raster`）只負責把決策畫出來；到 Artix 換成 `wl_shm`／EGL，決策層一行不用改。

## 用法

```sh
python3 minireader.py sample.html --width 72 --stats
python3 minireader.py https://www.gnu.org/ --width 66 --no-chrome --no-color
python3 naive.py minified.html        # 看「不做過濾」的下場
```

`--width` 欄寬／`--no-color` 純文字／`--no-chrome` 剝掉 nav-header-footer-aside／`--stats` 統計。

## 四段各自做了什麼

1. **fetch**：`urllib`，帶 UA，上限 2 MB，認 charset。
2. **filter**（真正的核心）：
   - 丟：`script`／`style`／`noscript`／`svg`／`canvas`／`template`
   - 剝：`nav`／`header`／`footer`／`aside`（可選）
   - 抽：p／h1-h6／li（含層級）／blockquote／pre／tr-td／img／video-audio-source
   - 行內：b／i／code 轉樣式位，`a` 收 href 進連結表
3. **layout**：
   - 顯示寬度計算：`east_asian_width`（CJK 佔 2 欄）
   - 換行原子：CJK 單字、拉丁詞、空白、硬換行
   - 懸掛縮排（清單）、`│` 引用前綴、`pre` 不重排
   - 禁則：`，。、；：！？）》」』】` 黏進前一原子，不置行首
   - 超長原子（網址）按顯示寬硬切
   - 表格：先量各欄最大寬再對齊，表頭加 `─┼─`
4. **render**：ANSI 粗體／斜體／底線／青色，相鄰同樣式合併（少發轉義碼）；沒 tty 就純文字。

## 實測（這台是手機 iSH 模擬層，數字不代表物理機）

| 樣本 | 大小 | 塊 / 行 | 過濾 | 排版 |
|---|---|---|---|---|
| `sample.html` | 1.5 KB | 23 / 34 | 16 ms | — |
| `gnu.org`（真實站，含抓取 1.8 s） | 31 KB | 109 / 241 | 418 ms | 合計 |
| 合成 40 份 | 51 KB | 800 / 1239 | 999 ms | 323 ms |
| 合成 160 份 | 202 KB | 3200 / 4959 | 4039 ms | 1401 ms |

**結論：解析是成本大頭，排版是零頭。** 純 Python 的 `HTMLParser` 線性但不快；真實站（1–3 MB，九成是 JS/CSS）要先丟再解析，或換 C 解析器。

## 對照組（為什麼需要過濾器）

`minified.html` 壓成一行後，`naive.py`：

```
壓成一行的真實站首頁新聞站點標題第一段文字，跟標題黏在一起也看不出來。第二段文字…© 2026
```

全黏成一坨，選單與正文不分、清單消失、影片蒸發。

`minireader.py` 同一份：

```
首頁新聞
站點標題
第一段文字，跟標題黏在一起也看不出來。
• 清單一
〔媒體 video〕https://cdn.example.com/a.mp4  →  mpv
```

## 已知邊界（誠實清單）

- **不跑 JS**：SPA、無限滾動、驗證頁 → 空白。這是設計取捨，不是 bug。
- **不讀 CSS**：`display:none` 的內容照樣輸出、兩欄排版壓成單欄、`float` 不存在。
- **表格**：只做「每列格數相同時」的對齊，colspan/rowspan 直接放棄。
- **禁則**：只做「標點不置行首」，沒做行末禁則、沒做拉丁文斷字。
- **內文抽取**：目前是「全選」，沒有正文識別（Readability 那套）。真實站要先剝 nav，不然讀者模式會混進選單。
- **媒體**：只認 `src`／`data-src`，不處理 `<source>` 的多格式、HLS/DASH 清單、DRM。
  真實站點更麻煩：`<video>` 的 `src` 常常是 `blob:`（JS 組出來的）或帶簽名的 `.m3u8`，B站／YouTube 甚至要整頁解析。
  所以「給連結」這一步遲早要接 **yt-dlp**（mpv 內建 `--ytdl`），否則只能播直鏈。
- **效能**：手機模擬層上 200 KB 要 4 s，日常用不可接受；要嘛截斷，要嘛換解析器。

## 下一步（三選一）

1. **加正文識別**：按 `<article>`／class 權重打分，只留主文（最省力、收益最大）。
2. **加渲染後端**：終端之外再接一個 GTK4/Slint 視窗，驗證同一份「行串列」能餵兩種 renderer。
3. **換解析器**：用 `html.parser` 之外的流式 tokenizer（或 C 的 lexbor）壓時間。
