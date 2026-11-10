#!/bin/sh
# 階段 2：切到 tty2 跑 KMS 彩條，結束後自動切回 tty1
# 用法：sudo /home/user17/mrp2-bench/kms-test.sh [card] [hz]
BIN="${KMS_BIN:-/home/user17/mrp2-bench/bin/kms}"
DEV="${1:-/dev/dri/card2}"
HZ="${2:-120}"

trap 'chvt 1' EXIT INT TERM

echo "== 切到 tty2 =="
chvt 2 || { echo "chvt 2 失敗"; exit 1; }
sleep 2

echo "== 執行 kms（逾時 30 秒）=="
if [ -n "${KMS_FRAMES:-}" ]; then
    timeout 40 "$BIN" "$DEV" "$HZ" "$KMS_FRAMES"
else
    timeout 40 "$BIN" "$DEV" "$HZ"
fi
rc=$?
echo "== kms 結束 rc=$rc =="

echo "== 切回 tty1 =="
