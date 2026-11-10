# 交接件｜自研渲染協議 MRP2（新工作區用）

- 日期：2026-11-10
- 用途：把 `minireader` 這個 PoC 裡的渲染協議抽出，移交到**新工作區**繼續做
- 邊界：第一工作區（現有 Artix/Kubuntu 桌面）**繼續使用顯卡原生協議（X11／Wayland＋GL）**，
  本協議只在新工作區演進，不影響既有環境
- 狀態：協議 v2 已完成並在 iSH 驗證；KMS 輸出端只到「可編譯」，未上真機

---

## 0. 一句話

> **文字走「墨段」、影片走「共享記憶體」、能直掃就別合成。**
> 客戶端只說「哪裡有東西」，合成器負責變成像素。

---

## 1. 已完成 / 未完成

| 模組 | 狀態 | 驗證方式 |
|---|---|---|
| 協議編解碼（MRP2） | ✅ 完成 | `test_proto.py` 17/17 |
| 合成決策（damage／遮擋／直掃） | ✅ 完成 | `test_compositor.py` 28/28 |
| 輸入（evdev → 焦點／事件） | ✅ 完成 | `test_evdev.py` 18/18 |
| B 站連結正規化 | ✅ 完成 | `test_bili.py` 11/11 |
| 媒體解析（yt-dlp 包裝） | ✅ 完成 | `test_resolver.py` 14/14 |
| C 版伺服器（解碼＋決策＋光柵） | ✅ 可跑 | `csrc/mrpd.c` |
| 共享記憶體影片（PLANE） | ✅ 可跑 | `csrc/shmvid.c`、`demo_shm.py` |
| 點陣字＋TEXT op | ✅ 可跑 | `mkfont.py`、`font.rom` |
| DRM/KMS 輸出 | ⚠️ 僅編譯 | `kms/kms.c`（iSH 無 `/dev/dri`） |
| GPU 合成（EGL/GLES） | ⚠️ 骨架 | `gpu/glquad.c` 在 llvmpipe 驗過 API 路徑 |
| 三緩衝＋fence | ❌ 未做 | 需真 KMS 事件 |
| 多輸出／剪貼簿／IME | ❌ 未做 | — |

---

## 2. 檔案地圖

```
poc/minireader/
├─ PROTOCOL.md          ← 協議權威文件（先讀這份）
├─ proto.py             MRP2 編解碼（v1 兼容）
├─ compositor.py        合成決策核心（surface／damage／焦點／layer／節流）
├─ server.py            協議 → 合成 → 顯示
├─ display.py           顯示後端（Mock 可跑／DRM 骨架）
├─ evdev.py             /dev/input → KEY/POINTER
├─ shell.py             系統入口：輸入 → 協議 → 60/120 Hz
├─ render.py            軟體光柵＋PNG（Python 參考實作）
├─ minireader.py        HTML → 塊 → 行 → display list
├─ reader.py            論壇式前端（列表／跟隨／播放）
├─ tui.py               curses 前端
├─ resolver.py / bili.py  B 站與一般媒體解析
├─ mkfont.py            Unifont → 字模 ROM
├─ glyphfont.py         Python 點陣渲染（對照組）
├─ demo_proto.py        協議全鏈路 demo
├─ demo_video.py / demo_video_real.py  影片（假／真 ffmpeg）
├─ csrc/mrpd.c          C 版伺服器（協定＋合成＋光柵＋字模）
├─ csrc/shmvid.c        共享記憶體影片（XRGB memcpy／零拷貝）
├─ kms/kms.c            DRM/KMS 最小輸出端
├─ bench/               光柵／決策／1% low 基準
└─ test_*.py            88 條測試
```

---

## 3. 架構

```
App ──MRP2──▶ Server ──Compositor.plan()──▶ 光柵 ──▶ Display.flip()
  │              │
  │              └─ damage／遮擋／layer／直掃
  └─ 文字：SPANS 或 TEXT（字形 ID）
     影片：PLANE（共享記憶體 XRGB）
     點陣：BUFFER（16 px 橫帶，RGB565，過渡用）

evdev ──▶ Seat ──▶ KEY／POINTER／FOCUS（只給焦點）
```

---

## 4. 協議摘要（細節見 PROTOCOL.md）

| op | 方向 | 用途 |
|---|---|---|
| `HELLO` / `HELLO_ACK` | 雙向 | 版本與能力協商 |
| `SURF` | C→S | 幾何＋role（background/toplevel/panel/overlay/popup） |
| `SPANS` | C→S | 每 16 px tile 的墨段（文字主力） |
| `TEXT` | C→S | UTF-8 字串＋座標，伺服器查字模（v1.2 新增） |
| `PLANE` | C→S | 共享記憶體 XRGB（影片，v2.2） |
| `BUFFER` | C→S | 16 px 橫帶 RGB565（過渡） |
| `DAMAGE` / `COMMIT` / `UNMAP` | C→S | 髒區／批次結束／隱藏 |
| `FRAME` | S→C | 幀回呼（節拍） |
| `KEY` / `POINTER` / `FOCUS` | S→C | 輸入（只送焦點） |
| `CONFIGURE` / `ERROR` | S→C | 尺寸／錯誤 |

封包：`magic 'MRP2' | ver | flags | pad | nops` ＋ `type(1) len(2) payload`；
little-endian、無填充；未知 type 依 `len` 跳過（向前兼容）。

---

## 5. 實測數據（iSH，非物理機）

| 項目 | 數字 |
|---|---|
| 解析 159 KB HTML | 360 ms（Python） |
| 合成決策（200 surface／50 damage） | C 0.24 ms ／ Python 271 ms |
| 全屏光柵 1080p | C 11 ms（首幀）／Python 87 ms |
| 直掃 | 0.025 ms/幀，p99 0.165 ms |
| XRGB memcpy 1080p | 1.68 ms（iSH） |
| 文字頁協議 | span 2 524 B ／ TEXT 2 335 B |
| 影片協議 | RGB565 112.7 KB/幀（320×180）；XRGB 7.9 MB/幀（1080p） |
| 字模 | 746 字形 30.4 KB；GB2312 全量 193 KB；CJK 656 KB |
| 1% low（視窗化 60 Hz） | 基準 max 14.7 ms／`--prewarm` 6.99／`--rows` 4.51，皆零漏拍 |

---

## 6. 環境限制（iSH 專屬，別帶去新工作區）

| 限制 | 影響 | 對策 |
|---|---|---|
| 無 `/dev/dri` | KMS 只能編譯不能跑 | 到 Artix 驗 |
| 無 `memfd_create` | 影片用檔案 mmap 代替 | Artix 換 memfd |
| 無 `SCM_RIGHTS`（EINVAL） | fd 傳遞不可用 | 同上 |
| 全盤 noexec，只有 `/var/tmp` 能執行 | 編譯產物要放 `/var/tmp` | 已寫進指令 |
| 模擬層 CPU | 所有數字打一折看 | 物理機重測 |

---

## 7. 到 Artix 的落地步驟

1. **輸出**：`pacman -S libdrm gcc` → 編 `kms/kms.c` → 在 TTY（非圖形界面）跑
   - 內核參數 `nvidia_drm.modeset=1`（HDMI 在 N 卡上）
   - 若 `CREATE_DUMB` 回 ENOSYS → 改 GBM（`gbm_bo_create` + `drmModeAddFB2`）
2. **輸入**：`/dev/input/event*` 權限（video 群組）→ `evdev.py` 或移植 C
3. **合成**：把 `mrpd.c` 的 `save_ppm` 換成 `drmModePageFlip`（雙／三緩衝＋`FLIP_COMPLETE`）
4. **協定**：client 端還在 Python；之後可把 `proto.py` 移植成 C（佈局已固定，直讀 struct）
5. **GPU（可選）**：EGL surfaceless/GBM → `GLRenderer`（quad batch＋MSDF）

驗收順序：`kms` 出彩條 → `mrpd` 出文字頁 → 影片 PLANE → 鍵鼠焦點 → 幀率 60/120。

---

## 8. 已知坑（按踩到機率排序）

1. **N 卡 dumb buffer**：`DRM_IOCTL_MODE_CREATE_DUMB` 可能 ENOSYS → GBM 備案。
2. **VT 切換**：遊戲独占全屏要 `drmDropMaster` ＋ `VT_ACTIVATE`；回來 damage 全屏重畫。
3. **色彩空間**：B 站 BT.709 limited range，YUV 直通時別忘了矩陣。
4. **標題列 22 px 是寫死的**（協議債）——文字內容原點應該由 `CONTENT` op 指定。
5. **TEXT 座標是絕對的**，沒有 per-surface 裁剪；多視窗要自己算。
6. **行距**：16 px 字配 20 px 行距太擠，正式用 22–24。
7. **中文字模缺字**：只烘了文章字＋500 常用字；要完整就 193 KB（GB2312）。

---

## 9. 決策紀錄（為什麼這樣選）

| 決策 | 理由 | 被否決的方案 |
|---|---|---|
| 像素格式統一 XRGB8888 | 與 KMS 對齊，零轉換；RGB565 轉換反而慢 9× | RGB565 直送 |
| 文字用 span／TEXT，不用點陣 buffer | 頻寬小 900×，且能精確髒區 | 整張 UI buffer 上傳 |
| 點陣字而非向量字 | 合成器不用排版／度量／反鋸齒；像素可重現 | FreeType + MSDF（留給 GPU 階段） |
| 影片走共享記憶體 | 零拷貝；socket 傳不動 7.9 MB/幀 | 內嵌像素資料 |
| 合成器核心用 C、工具用 Python | 實測差 1400×；但開發速度 Python 快 | 全 C 或全 Python |
| 遊戲走直掃／VT 切換 | 遊戲每幀全變，合成毫無收益 | 讓遊戲走協議 |

---

## 10. 新工作區開工指令

```sh
# 1) 複製
cp -r /workspace/poc/minireader ~/mrp2 && cd ~/mrp2

# 2) 跑測試（純標準庫，除 freetype-py）
python3 test_proto.py && python3 test_compositor.py && python3 test_evdev.py

# 3) 產生一頁並用 C 伺服器畫出來
python3 minireader.py sample.html --emit --width 72 > out/doc.json
python3 - <<'PY'
import json, proto
doc=json.load(open('out/doc.json'))
ops=[('surf',90,0,0,1920,1080,0,0,1,0,0)]
y=60
for ln in doc['lines']:
    t=''.join(s['t'] for s in ln['s'])
    if t.strip(): ops.append(('text',1,60,y,4,t))
    y+=20
open('out/text.wire','wb').write(proto.encode(ops))
PY
gcc -O2 -o /var/tmp/mrpd csrc/mrpd.c && \
/var/tmp/mrpd out/text.wire out/text.ppm 1 1920 1080 --font out/font.rom
python3 gpu/ppm2png.py out/text.ppm out/text.png
```

依賴：`apk add mesa-dev mesa-dri-gallium ffmpeg font-unifont`、
`pip install freetype-py yt-dlp`（可選）。

---

## 11. 下一步優先序（新工作區）

1. **KMS 接線**：`mrpd` 直接 page flip（先 60 Hz，再 120）。
2. **三緩衝＋fence**：`FLIP_COMPLETE` 回收 buffer，量漏拍。
3. **GB2312 全量字模**：193 KB，順手把行距修好。
4. **PLANE 換 memfd**：正式 fd 傳遞，取代檔案 mmap。
5. **YUV 直通**：省掉解碼器那頭的 RGB 轉換。
6. **GL 合成器**：quad batch＋MSDF，解鎖 260 Hz。

---

## 12. 交接自檢

- [x] 協議有權威文件（PROTOCOL.md），與 `proto.py` 一致
- [x] 88 條測試可重跑，無外部服務依賴
- [x] 所有數字標明測量環境（iSH 模擬層）
- [x] 未完成項明確標示（KMS、fence、多輸出、IME）
- [x] 決策紀錄含被否決方案，避免新工作區重複踩坑
