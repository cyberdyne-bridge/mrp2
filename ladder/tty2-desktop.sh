#!/bin/bash
# tty2-desktop.sh — 在 tty2 顯示完整桌面複刻，帶游標／靜止偵測
#
# 用法：sudo ./tty2-desktop.sh [mouse|scripted] [秒數]
#   mouse    真實滑鼠（預設）：移動 → 只重繪游標區域；停 3 秒 → 定格
#   scripted 腳本游標：活動 0-6s、11-17s，其餘靜止（自動示範）
#
# 一律經 run-safe.sh：硬逾時、行程組終止、結束保證切回 tty1。
set -u
cd "$(dirname "$0")"

if [ "$(id -u)" != 0 ]; then
    exec sudo "$0" "$@"
fi

MODE="${1:-mouse}"
SECS="${2:-120}"
ARGS=(--cursor --seconds "$SECS")
[ "$MODE" = mouse ] && ARGS+=(--mouse)

exec ./run-safe.sh $((SECS + 20)) \
    ./bin/mrpd_kms ./assets/tty2desktop.wire 0 120 \
    --font ./assets/font16.rom "${ARGS[@]}"
