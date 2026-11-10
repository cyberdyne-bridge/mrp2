#!/bin/bash
# run-safe.sh — 在 tty2 安全執行 KMS 程式
#
# 用法：sudo run-safe.sh <最大秒數> <命令...>
#
# 保證：
#   1. 整個命令有硬超時（看門狗），不依賴內層程式自己結束
#   2. 用 setsid 讓命令自成一行程組，逾時時對「整個群組」送 SIGTERM
#   3. 只有先切回 tty1、釋放 DRM master 之後，才可能用到 SIGKILL
#   4. 無論成功、失敗、逾時，結束時一定 chvt 1
#   5. 結束後驗證 sway 輸出是否真的恢復，沒恢復就 reload 並明確告警
set -u

MAX="${1:?用法: run-safe.sh <秒數> <命令...>}"
shift
U="user17"
RT="/run/user/1000"

sway_out_ok() {
    local sock
    sock=$(ls "$RT"/sway-ipc.*.sock 2>/dev/null | head -1)
    [ -n "$sock" ] || return 1
    runuser -u "$U" -- env XDG_RUNTIME_DIR="$RT" SWAYSOCK="$sock" \
        swaymsg -t get_outputs 2>/dev/null | grep -q '"active": true'
}

restore() {
    chvt 1 2>/dev/null
    for _ in $(seq 1 40); do
        sway_out_ok && { echo "[run-safe] 桌面已恢復" >&2; return 0; }
        sleep 0.25
    done
    echo "[run-safe] 警告：sway 輸出未恢復，嘗試 reload" >&2
    local sock
    sock=$(ls "$RT"/sway-ipc.*.sock 2>/dev/null | head -1)
    [ -n "$sock" ] && runuser -u "$U" -- env XDG_RUNTIME_DIR="$RT" SWAYSOCK="$sock" \
        swaymsg reload >/dev/null 2>&1
    sleep 1
    if sway_out_ok; then
        echo "[run-safe] reload 後已恢復" >&2
    else
        echo "[run-safe] 仍失敗！請切到 tty1 手動檢查或重啟" >&2
    fi
}

trap 'restore' EXIT INT TERM

echo "[run-safe] 切到 tty2，上限 ${MAX}s：$*" >&2
chvt 2 || { echo "[run-safe] chvt 2 失敗" >&2; exit 1; }
sleep 1

# setsid：命令成為新行程組的 leader，逾時可整組終止
setsid "$@" &
PID=$!

(
    sleep "$MAX"
    if kill -0 "$PID" 2>/dev/null; then
        echo "[run-safe] 逾時 ${MAX}s，先對行程組送 SIGTERM" >&2
        kill -TERM -- "-$PID" 2>/dev/null
        sleep 1
        if kill -0 "$PID" 2>/dev/null; then
            echo "[run-safe] 仍在，先 chvt 1 釋放 DRM master 再 SIGKILL" >&2
            chvt 1 2>/dev/null
            sleep 0.5
            kill -KILL -- "-$PID" 2>/dev/null
        fi
    fi
) &
WD=$!

wait "$PID"; RC=$?
kill "$WD" 2>/dev/null
wait "$WD" 2>/dev/null

restore
trap - EXIT
echo "[run-safe] 命令結束 rc=$RC" >&2
exit "$RC"
