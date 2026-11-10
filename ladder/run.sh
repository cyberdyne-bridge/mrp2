#!/usr/bin/env bash
# MRP2 階段 1 — 離屏驗證
#
# 只跑協議 / 合成 / 光柵 / 測試，**不碰 /dev/dri、不碰 DRM/KMS**，
# 所以完全不會影響第一工作區的 sway 桌面。
set -u

SRC="${MRP2_SRC:-$HOME/mrp2}"
DIR="$HOME/mrp2-bench"
BIN="$DIR/bin"
OUT="$DIR/out"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$OUT/run-$STAMP.log"
REPORT="$DIR/RESULTS-$STAMP.md"

mkdir -p "$BIN" "$OUT/demo" "$OUT/comp"

exec > >(tee -a "$LOG") 2>&1

say() { printf '%s\n' "$*"; }
sec() { say ""; say "── $* ──"; }

say "MRP2 階段 1 離屏驗證  $(date '+%F %T')"
say "來源: $SRC"

# ── 環境 ────────────────────────────────────────────────
sec "環境"
CPU_MODEL=$(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //')
say "CPU     : $CPU_MODEL  ($(nproc) 執行緒)"
say "Kernel  : $(uname -r)"
say "Python  : $(python3 -V 2>&1)"
say "gcc     : $(gcc --version | head -1)"
say "libdrm  : $(pacman -Q libdrm 2>/dev/null || echo '未安裝')"

# ── 編譯 ────────────────────────────────────────────────
sec "編譯"
ok=1
gcc -O2 -o "$BIN/raster_c" "$SRC/bench/raster.c" && say "raster.c  OK" || { say "raster.c  FAIL"; ok=0; }
gcc -O2 -o "$BIN/plan_c"   "$SRC/bench/plan.c"   && say "plan.c    OK" || { say "plan.c    FAIL"; ok=0; }
gcc -O2 -o "$BIN/mrpd"     "$SRC/csrc/mrpd.c"    && say "mrpd.c    OK" || { say "mrpd.c    FAIL"; ok=0; }
gcc -O2 -o "$BIN/kms"      "$SRC/kms/kms.c" -I/usr/include/libdrm -ldrm \
    && say "kms.c     OK（僅編譯，不執行）" || say "kms.c     FAIL（缺 libdrm？）"
[ "$ok" = 1 ] || { say "有編譯失敗，中止"; exit 1; }

# ── 測試 ────────────────────────────────────────────────
sec "測試套件"
PASS_TOTAL=0; PASS_OK=0
for t in test_proto.py test_compositor.py test_evdev.py test_bili.py test_resolver.py; do
    out=$(cd "$SRC" && timeout 180 python3 "$t" 2>&1) || true
    last=$(printf '%s\n' "$out" | grep -E '通過' | tail -1)
    say "- $t : ${last:-（無輸出）}"
done

# ── 光柵基準 ────────────────────────────────────────────
sec "光柵基準（1232×1488，取最佳）"
C_RASTER=$("$BIN/raster_c" 2>&1)
say "$C_RASTER"
P_RASTER=$(cd "$SRC/bench" && timeout 300 python3 raster.py 2>&1)
say "$P_RASTER"

# ── 合成決策基準 ────────────────────────────────────────
sec "合成決策基準（200 surface / 50 damage / 500 幀）"
C_PLAN=$("$BIN/plan_c" 2>&1)
say "$C_PLAN"
P_PLAN=$(PYTHONPATH="$SRC" timeout 600 python3 "$SRC/bench/plan_bench.py" 2>&1)
say "$P_PLAN"

# ── C 伺服器渲染 ────────────────────────────────────────
sec "C 伺服器（mrpd）渲染 1080p 文字頁"
MRPD=$("$BIN/mrpd" "$SRC/assets/text.wire" "$OUT/text.ppm" 1 1920 1080 \
        --font "$SRC/assets/font.rom" 2>&1)
say "$MRPD"
python3 "$SRC/gpu/ppm2png.py" "$OUT/text.ppm" "$OUT/text.png" || true

# ── 協議全鏈路 demo ─────────────────────────────────────
sec "協議全鏈路（client → MRP2 → server → PNG）"
(cd "$SRC" && timeout 120 python3 minireader.py sample.html --emit --width 72 > "$OUT/demo/doc.json") || true
DEMO=$(cd "$SRC" && timeout 180 python3 demo_proto.py "$OUT/demo" 2>&1)
say "$DEMO"

# ── 合成器 demo ─────────────────────────────────────────
sec "合成器 demo（焦點 / layer / 節流 / 直掃）"
COMP=$(cd "$SRC" && timeout 120 python3 compositor.py --demo "$OUT/comp" 2>&1)
say "$COMP"

# ── 摘要 ────────────────────────────────────────────────
pval() { printf '%s\n' "$1" | awk -v k="$2" '$1==k {print $2; exit}'; }
c_clear=$(pval "$C_RASTER" clear); c_solid=$(pval "$C_RASTER" solid); c_ink=$(pval "$C_RASTER" ink)
p_clear=$(pval "$P_RASTER" clear); p_solid=$(pval "$P_RASTER" solid); p_ink=$(pval "$P_RASTER" ink)
c_plan=$(printf '%s\n' "$C_PLAN" | awk '/每幀/{print $6; exit}')
p_plan=$(printf '%s\n' "$P_PLAN" | awk '/每幀/{print $6; exit}')

cat > "$REPORT" <<EOF
# MRP2 階段 1 結果 — $STAMP

- 機器：$CPU_MODEL / $(nproc) 執行緒 / $(uname -r)
- Python $(python3 -V 2>&1 | sed 's/Python //')
- 模式：**純離屏**（無 DRM/KMS，未影響桌面）

## 光柵（1232×1488，越低越好）

| 操作 | C(-O2) | Python | C/Python |
|---|---|---|---|
| clear | ${c_clear:-?} ms | ${p_clear:-?} ms | $(awk -v a="${c_clear:-1}" -v b="${p_clear:-1}" 'BEGIN{if(a>0)printf "%.0f×", b/a}') |
| solid | ${c_solid:-?} ms | ${p_solid:-?} ms | $(awk -v a="${c_solid:-1}" -v b="${p_solid:-1}" 'BEGIN{if(a>0)printf "%.0f×", b/a}') |
| ink   | ${c_ink:-?} ms | ${p_ink:-?} ms | $(awk -v a="${c_ink:-1}" -v b="${p_ink:-1}" 'BEGIN{if(a>0)printf "%.0f×", b/a}') |

## 合成決策（200 surface / 50 damage / 500 幀）

| 實作 | 每幀 |
|---|---|
| C(-O2) | ${c_plan:-?} ms |
| Python | ${p_plan:-?} ms |

## 對照 iSH（交接件數字）

| 項目 | iSH | 本機 |
|---|---|---|
| C 光柵 solid | 0.23 ms | ${c_solid:-?} ms |
| Python 光柵 solid | 319.00 ms | ${p_solid:-?} ms |
| C 合成決策 / 幀 | 0.24 ms | ${c_plan:-?} ms |
| Python 合成決策 / 幀 | 271 ms | ${p_plan:-?} ms |

原始日誌：\`$LOG\`
EOF

say ""
say "完成。報告：$REPORT"
say "原始日誌：$LOG"
