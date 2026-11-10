#!/bin/bash
# play-local.sh — 播放 /tmp/mrp-bili.mp4（由 run-safe.sh 以 root 執行）
VIDEO="${MRP_VIDEO:-/tmp/mrp-bili.mp4}"
FRAMES="${MRP_FRAMES:-300}"
AUDIO="${MRP_AUDIO:-plughw:0,0}"
exec ffmpeg -re -hide_banner -loglevel warning -i "$VIDEO" \
  -map 0:v:0 -vf scale=1920:1080 -pix_fmt nv12 -f rawvideo pipe:1 \
  -map 0:a:0 -f alsa -ac 2 -ar 48000 "$AUDIO" 2>/tmp/mrp-ff.log \
  | /home/user17/mrp2-bench/bin/nv12_ui /dev/dri/card2 120 "$FRAMES"
