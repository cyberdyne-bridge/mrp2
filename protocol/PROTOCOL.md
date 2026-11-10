# MRP2 — 系統級渲染協議 v2

> 跟 X11／Wayland **平級**：不是某個 App 的私有格式，而是這個系統的顯示協議。
> 桌布、面板、播放器、閱讀器、任何 App 都接同一套。
> 核心信條：**客戶端只提交有墨的地方，黑底不上線。**

`proto.py` 是本文件的實作；`server.py` 是參考伺服器；`kms/kms.c` 是最小輸出端。

---

## 0. 這東西在系統裡的位置

```
        App A          App B         桌布／面板
          │              │              │
          └──────── MRP2（本協議）───────┘
                          │
                   ┌──────┴──────┐
                   │   Server    │  合成、焦點、幀節流
                   └──────┬──────┘
                          │
              ┌───────────┴───────────┐
        DRM/KMS（page flip）      evdev（鍵鼠）
```

X11／Wayland 做的事它都做：宣告 surface、收輸入、同步幀、管焦點。
不同的是它**沒有「整張 buffer 交換」這個概念**——文字走 span，圖片與影片才走 buffer（v3）。

---

## 1. 設計原則

| 原則 | 做法 | 收益 |
|---|---|---|
| 只傳墨 | `SPANS` 每 tile 只送有內容的水平段 | 同內容 913× 頻寬 |
| 合成器不排版 | 斷行、座標、字形都在客戶端 | 伺服器小、可審計 |
| 髒區精確 | `SPANS` 自帶 tile → 只髒那一條 | 改一行只合成 8 K 像素 |
| 二進位固定佈局 | little-endian、無填充 | C 端可 `struct` 直讀 |
| 向前兼容 | 未知 op 依 `len` 跳過 | 前後端可不同版本 |
| 同步明確 | `COMMIT` + `FRAME` 回呼 + `serial` | 不會一邊畫一邊改 |

---

## 2. 傳輸與分幀

- 載體：Unix socket（`SOCK_SEQPACKET` 最佳；`SOCK_STREAM` 需 4 B 長度前綴）、pipe、本地 TCP。
- 雙向：同一條連線上跑 client→server 的 op 與 server→client 的 event，用 type 區分（`0x01–0x7F` 上行、`0x80–0xFF` 下行）。
- 一個封包可以含多個 op；`COMMIT` 之前的所有更新屬於同一批。
- little-endian、緊密排列、無對齊填充。

## 3. 封包頭（11 B）

| 偏移 | 長度 | 型別 | 欄位 |
|---|---|---|---|
| 0 | 4 | char[4] | magic：`MRP2`（v1 為 `MRP1`，仍可解） |
| 4 | 1 | u8 | version（2） |
| 5 | 1 | u8 | flags（保留） |
| 6 | 1 | u8 | reserved |
| 7 | 4 | u32 | nops |

## 4. Op 通用頭（3 B）

`type:u8, len:u16`，隨後 `len` bytes payload。

---

## 5. Client → Server

### 5.1 HELLO（6 B）

| 欄位 | 型別 | 說明 |
|---|---|---|
| proto | u8 | 客戶端最高版本（2） |
| caps | u32 | 能力位元 |

caps：`SPANS=1`、`SHM=2`、`DMABUF=4`、`FRAME_CALLBACK=8`。

### 5.2 SURF（17 B）

| 欄位 | 型別 | 說明 |
|---|---|---|
| id | u16 | 1–65535 |
| x, y | i16 | 輸出座標 |
| w, h | u16 | 尺寸 |
| z | i16 | 同角色內堆疊序 |
| layer | u8 | 舊欄位（v1 相容），v2 以 role 為準 |
| flags | u8 | bit0 OPAQUE、bit1 HIDDEN、bit2 FOCUSABLE |
| role | u8 | 0 background、1 toplevel、2 panel、3 overlay、4 popup |
| parent | u16 | popup 的父 surface，其餘為 0 |

角色決定層：`background < toplevel < panel < overlay`。
panel 預設不吃鍵盤焦點；background 不參與命中。

### 5.3 DAMAGE（10 B）

`id:u16, x:i16, y:i16, w:u16, h:u16`（surface 局部座標）

### 5.4 SPANS（8 + 7n）

`id:u16, tilex:u16, tiley:u16, n:u16` + `n × SPAN`

SPAN（7 B）：`y:u16（tile 內 0–15）, x0:u16, x1:u16, color:u8`

tile 高度固定 **16 px**。同一列的多段可合併，伺服器不檢查重疊。

### 5.5 COMMIT / UNMAP（各 2 B）

`id:u16`。`COMMIT` 只代表「這批送完」，**不膨脹髒區**。
若客戶端宣告了 `FRAME_CALLBACK`，伺服器回一個 `FRAME` 事件。

### 5.6 FRAME_ACK（6 B）

`id:u16, serial:u32` — 告知伺服器「這幀我畫完了」，用於幀預算統計（v2 僅記錄）。

---

## 6. Server → Client

| type | 名稱 | payload | 用途 |
|---|---|---|---|
| 0x81 | HELLO_ACK | 15 B | proto, caps, serial, w, h, refresh |
| 0x82 | KEY | 12 B | surface, serial, keysym, state, mods |
| 0x83 | POINTER | 12 B | surface, x, y, button, state, serial |
| 0x84 | FOCUS | 3 B | surface, state |
| 0x85 | FRAME | 6 B | surface, serial |
| 0x86 | CONFIGURE | 10 B | surface, w, h, serial |
| 0x87 | ERROR | 2 + n | code, msg（UTF-8） |

**KEY／POINTER 只送給焦點 surface**；`surface=0` 表示「沒人接」。
`serial` 單調遞增，客戶端可用它對齊自己的狀態機。

---

## 7. 握手

```
client → HELLO(proto=2, caps)
server → HELLO_ACK(proto=2, caps, serial=0, w, h, refresh)
client → SURF … SPANS … COMMIT
server → FRAME(surface, serial)
```

- 版本不符 → server 回 `ERROR(1, "version")` 並斷線。
- 沒握手就送 op：v2 容忍（視為 caps=0），但不會有幀回呼。

---

## 8. 幀同步

- 伺服器以輸出刷新率為格線（60 Hz → 16.67 ms），同一格內多次 `COMMIT` 合併成一幀。
- 每幀結束送 `FRAME(surface, serial)`；客戶端可用 `FRAME_ACK` 回報耗時。
- 髒區為空 → 不送 `FRAME`（跳幀），客戶端不該等。

實測：200 ms 內 101 次更新 → 13 幀（60 Hz 上限即 13）。

---

## 9. 輸入模型（v2）

- 單一 **seat**（v3 才多 seat）。
- 點擊：由上而下命中（`layer` 高的優先；`FOCUSABLE` 且可見）；改焦點並送 `FOCUS`。
- 鍵盤：只送焦點 surface；沒有焦點則 `surface=0`。
- 修飾鍵（mods）與 keysym（X11 keysym 數值）暫用 XKB 表，v3 換成自家 keymap。

---

## 10. 座標與內容原點

- `SURF.x/y`：surface 在輸出上的左上角。
- `SPAN.x0/x1`：相對 surface 左上。
- `SPAN.y`：`tiley × 16 + y`，相對 surface 內容原點。

**已知醜處**：v2 的內容原點固定是 `y = 22`（讓出標題列），
這是伺服器實作約定、不是協議欄位。v3 加 `CONTENT` op 或 flags 位。

---

## 11. 生命週期

```
HELLO ─▶ SURF ─▶ SPANS* ─▶ COMMIT ─▶ FRAME
                     ▲                    │
                     └── 只送變動的 tile ──┘
SURF 同 id = 更新幾何（髒新舊兩塊）
UNMAP = 隱藏；再 SURF 同 id 可復活（內容保留）
```

---

## 12. 上限與錯誤

| 項目 | 上限 |
|---|---|
| payload | 65 535 B |
| 每 op span 數 | 9 361 |
| surface 數 | 65 535 |
| 座標 | ±32 767 |
| 內容高度 | 1 048 560 px |

錯誤處理：magic／版本／長度不符 → 丟棄整包、連線保留；未知 type → 跳過；
未知 id → 靜默丟棄。

---

## 13. 跟 X11／Wayland 對照

| MRP2 | X11 | Wayland | 差異 |
|---|---|---|---|
| `SURF` | CreateWindow / ConfigureWindow | xdg_surface + configure 往返 | 幾何直接給，無握手 |
| `SPANS` | PutImage（整塊像素） | `wl_buffer`（整塊像素） | **只傳墨** |
| `COMMIT` | 無（即時） | `wl_surface.commit` | 同義 |
| `FRAME` | 無 | `wl_callback.done` | 同義 |
| `role` | override-redirect / 類型提示 | xdg_toplevel / layer-shell | 內建四層 |
| KEY/POINTER | XSendEvent | wl_seat | 同義，單 seat |
| 整張 buffer | 必須 | 必須 | **不是必須** |

代價：所有 App 要重寫，沒有 XWayland 兼容層。

---

## 14. 60 Hz 預算（1920×1080，先 CPU）

一幀 = 1920×1080×4 = **8.29 MB**；雙緩衝 16.6 MB；60 fps 需要 497 MB/s 寫入
（DDR4 是幾十 GB/s，完全不是瓶頸）。

16.67 ms 要分給：協議解析 → 合成決策 → 光柵 → page flip。

| 階段 | C | Python（iSH 實測） | 備註 |
|---|---|---|---|
| 協議解析 | < 0.1 ms | ~0.5 ms | 固定佈局 |
| 合成決策 | 0.24 ms | 271 ms（200 surface） | 決策要用 C |
| 光柵（全屏） | **0.23 ms** | 87 ms | Python 只適合畫髒區 |
| 光柵（8 K 髒區） | < 0.1 ms | 22 ms | |
| page flip | 等 vblank | 同 | 必然吃一格 |

結論：**1080p@60 用 C 是舒服的（< 1 ms），用 Python 全屏重繪不行（87 ms）**。
Python 留在協議／工具／測試層；伺服器核心用 C。

---

## 15. v3 待辦

1. `BUFFER`／`SHM`：圖片與影片幀（`memfd` + `SCM_RIGHTS`），v2 只留位。
2. 多輸出（`OUTPUT` op + 每輸出獨立幀）。
3. 剪貼簿與拖放。
4. 自訂 keymap／IME 通道（現在是 XKB keysym）。
5. popup 的真正父子約束（v2 只帶 parent id）。
6. tile 內 RLE（文字頁大片同色）。

---

## 16. 測試

```sh
python3 test_proto.py     # 17 項：往返、v1 兼容、握手、輸入、幀回呼、髒區、節流
python3 demo_proto.py out # 全鏈路 + 事件回送 + PNG
```

實測（960×600、44 行文件、315 span）：

| 項目 | 數字 |
|---|---|
| 首幀封包 | 2 572 B |
| 同內容 RGBA | 2 304 000 B（896×） |
| 改一行的封包 | 34 B |
| 改一行的合成 | 8 K 像素 |
