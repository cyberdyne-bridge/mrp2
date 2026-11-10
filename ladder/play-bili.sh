#!/bin/bash
# play-bili.sh — 安全播放 B 站影片
# 用法：./play-bili.sh <URL> [秒數，預設 12]
# 流程：有界下載到本地 → run-safe 本地播放（硬逾時 + 保證恢復桌面）
set -e
URL="${1:?用法: play-bili.sh <URL> [秒數]}"
SECS="${2:-12}"
OUT=/tmp/mrp-bili.mp4
AUDIO="${MRP_AUDIO:-plughw:0,0}"

rm -f "$OUT"
echo "[play-bili] 下載前 ${SECS} 秒 ..."
uvx yt-dlp --no-warnings --no-playlist \
  --download-sections "*0-$SECS" --force-keyframes-at-cuts \
  -f "bestvideo[height<=1080]+bestaudio/best" \
  --merge-output-format mp4 -o "$OUT" "$URL"

echo "[play-bili] 本地播放 ..."
sudo /home/user17/mrp2-bench/run-safe.sh $((SECS + 30)) \
  env MRP_FRAMES=$((SECS * 25)) MRP_AUDIO="$AUDIO" \
  /home/user17/mrp2-bench/play-local.sh
