#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""demo_video.py — 影片路徑最小可行版（BUFFER op）

不接解碼器：先自己生成 24 幀「假影片」（漸層＋移動方塊），
用 BUFFER 一帶一帶送進協議，證明影片真的能進合成器。

  python3 demo_video.py out/
"""
import os
import sys
import time

import proto
import server as srv
from display import MockDisplay

VW, VH = 320, 180
BAND = 16
FRAMES = 24


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def make_frame(i, w=VW, h=VH):
    """漸層底 ＋ 移動的方塊（越跑越右）。"""
    buf = bytearray(w * h * 2)
    bx = (i * 12) % (w - 40)
    for y in range(h):
        row = y * w * 2
        for x in range(w):
            if bx <= x < bx + 40 and h // 3 <= y < 2 * h // 3:
                v = rgb565(240, 180, 40)
            else:
                v = rgb565(20 + x * 200 // w, 30 + y * 180 // h, 120)
            p = row + x * 2
            buf[p] = v & 0xFF
            buf[p + 1] = v >> 8
    return buf


def frame_ops(sid, frame, w=VW, h=VH):
    ops = []
    for ty in range((h + BAND - 1) // BAND):
        y0 = ty * BAND
        band = frame[y0 * w * 2:(y0 + BAND) * w * 2]
        ops.append(('buffer', sid, ty, 0, w, min(BAND, h - y0), band))
    return ops


def main(argv=None):
    argv = argv or sys.argv[1:]
    outdir = argv[0] if argv else 'out'
    os.makedirs(outdir, exist_ok=True)

    W, H = 960, 600
    d = MockDisplay(W, H)
    s = srv.Server(W, H, d)
    s.feed(proto.encode([('surf', 9, 300, 120, VW, VH + 22, 5, 1, 0b101, 1, 0),
                         ('commit', 9)]))
    s.flush_events()

    total_bytes = 0
    t_enc = t_app = t_rast = 0.0
    for i in range(FRAMES):
        f = make_frame(i)
        t = time.time()
        wire = proto.encode(frame_ops(9, f))
        t_enc += time.time() - t
        total_bytes += len(wire)

        t = time.time()
        s.feed(wire)
        t_app += time.time() - t

        if i in (0, FRAMES // 2, FRAMES - 1):
            t = time.time()
            s.frame('%s/video_%02d.png' % (outdir, i))
            t_rast += time.time() - t

    rgba = VW * VH * 4
    print('影片 %dx%d × %d 幀 ／ 每幀 %d 帶' % (VW, VH, FRAMES, (VH + BAND - 1) // BAND))
    print('每幀協議 %.1f KB（RGB565）｜ 若送 RGBA %.1f KB（%.1f×）'
          % (total_bytes / FRAMES / 1024, rgba / 1024, rgba / (total_bytes / FRAMES)))
    print('平均：編碼 %.1f ms ／ 套用 %.1f ms ／ 光柵 %.0f ms（每幀）'
          % (t_enc / FRAMES * 1000, t_app / FRAMES * 1000, t_rast / 3 * 1000))
    return 0


if __name__ == '__main__':
    sys.exit(main())
