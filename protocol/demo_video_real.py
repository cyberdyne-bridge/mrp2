#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""demo_video_real.py — 真影片：ffmpeg 解碼 → BUFFER → 合成器 → PNG

  ffmpeg -f lavfi -i testsrc2=size=320x180:rate=30 -t 1 -c:v mjpeg -q:v 4 /tmp/t.avi
  python3 demo_video_real.py /tmp/t.avi out
"""
import os
import subprocess
import sys
import time

import proto
import server as srv
from display import MockDisplay

FFMPEG = '/usr/bin/ffmpeg'
VW, VH, BAND = 320, 180, 16


def main(argv=None):
    argv = argv or sys.argv[1:]
    if not argv:
        print(__doc__)
        return 2
    src = argv[0]
    outdir = argv[1] if len(argv) > 1 else 'out'
    os.makedirs(outdir, exist_ok=True)

    W, H = 960, 600
    s = srv.Server(W, H, MockDisplay(W, H))
    s.feed(proto.encode([('surf', 9, 300, 120, VW, VH + 22, 5, 1, 0b101, 1, 0),
                         ('commit', 9)]))
    s.flush_events()

    cmd = [FFMPEG, '-hide_banner', '-loglevel', 'error', '-i', src,
           '-f', 'rawvideo', '-pix_fmt', 'rgb565le', '-']
    t0 = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    fsize = VW * VH * 2
    bytes_in = 0
    nframes = 0
    t_apply = t_enc = 0.0
    t_rast = 0.0
    shots = {0, 15, 29}

    while True:
        frame = p.stdout.read(fsize)
        if len(frame) < fsize:
            break
        ops = []
        for ty in range((VH + BAND - 1) // BAND):
            y0 = ty * BAND
            band = frame[y0 * VW * 2:(y0 + BAND) * VW * 2]
            ops.append(('buffer', 9, ty, 0, VW, min(BAND, VH - y0), band))
        t = time.time()
        wire = proto.encode(ops)
        t_enc += time.time() - t
        bytes_in += len(wire)

        t = time.time()
        s.feed(wire)
        t_apply += time.time() - t

        if nframes in shots:
            t = time.time()
            s.frame('%s/real_%02d.png' % (outdir, nframes))
            t_rast += time.time() - t
        nframes += 1

    p.wait()
    wall = (time.time() - t0) * 1000
    print('解碼 %d 幀（%s）' % (nframes, os.path.basename(src)))
    print('總耗時 %.0f ms → %.1f fps（含 ffmpeg 解碼）' % (wall, nframes * 1000 / wall))
    print('協議 %.1f KB/幀（RGB565）｜ 解碼＋編碼＋套用 %.1f ms/幀'
          % (bytes_in / max(1, nframes) / 1024, (t_enc + t_apply) / max(1, nframes) * 1000))
    print('光柵 %.0f ms/幀（Python 逐像素；C 版會是 ms 以下）' % (t_rast / len(shots)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
