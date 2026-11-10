#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""demo_proto.py — MRP2 全鏈路（iSH 可跑）

client ──HELLO──▶ server ──HELLO_ACK──▶ client
client ──SURF/SPANS/COMMIT──▶ server ──FRAME──▶ client
server ──KEY/POINTER/FOCUS──▶ client

  python3 demo_proto.py out/
"""
import json
import os
import sys

import proto
import server as srv
from display import MockDisplay
from minireader import dw

CW, LH, GLYPH_H = 8, 16, 7


def color_index(kind, seg):
    if seg.get('h'):
        return 1
    if seg.get('c'):
        return 2
    if kind in ('h1', 'h2', 'h3', 'h4', 'h5', 'h6'):
        return 3
    if kind == 'quote':
        return 5
    if kind in ('rule', 'media', 'media-url', 'img'):
        return 4
    return 0


def doc_to_spans(doc, tile_h=16):
    tiles = {}
    y = 0
    for ln in doc['lines']:
        x = 0
        kind = ln['k']
        for seg in ln['s']:
            cols = sum(dw(c) for c in seg['t'])
            if cols and not seg.get('n'):
                ci = color_index(kind, seg)
                for dy in range(GLYPH_H):
                    yy = y + dy
                    tiles.setdefault(yy // tile_h, []).append(
                        (yy % tile_h, x * CW, (x + cols) * CW, ci))
            x += cols
        y += LH
    return tiles


def surf_op(sid, x, y, w, h, z=1, role=1, parent=0, flags=0b101):
    return ('surf', sid, x, y, w, h, z, 1, flags, role, parent)


def build_ops(doc, sid=1, x=40, y=60, w=520, h=380):
    ops = [surf_op(sid, x, y, w, h)]
    for ty, spans in sorted(doc_to_spans(doc).items()):
        ops.append(('spans', sid, 0, ty, spans))
    ops.append(('commit', sid))
    return ops


def main(argv=None):
    argv = argv or sys.argv[1:]
    outdir = argv[0] if argv else 'out'
    os.makedirs(outdir, exist_ok=True)
    doc = json.load(open('%s/doc.json' % outdir, encoding='utf-8'))

    W, H = 960, 600
    for a in argv:
        if a.startswith('--size='):
            W, H = [int(v) for v in a.split('=', 1)[1].split('x')]
    d = MockDisplay(W, H)
    s = srv.Server(W, H, d, refresh=60)

    # 1) 握手
    s.feed(proto.encode([('hello', 2, proto.CAP_SPANS | proto.CAP_FRAME_CALLBACK)]))
    ack = proto.decode(s.flush_events())[0]
    print('握手：%s（proto=%d caps=%d %dx%d@%dHz）'
          % (ack[0], ack[1], ack[2], ack[4], ack[5], ack[6]))

    # 2) 桌布 + 面板 + 兩個視窗
    wire = proto.encode([surf_op(90, 0, 0, W, H, z=0, role=0, flags=0b001)] +
                        build_ops(doc) +
                        [surf_op(2, 560, 300, 360, 240, z=2, role=2, flags=0b110),
                         ('commit', 2)])
    s.feed(wire)
    evs = proto.decode(s.flush_events())
    print('送 %d bytes → 幀回呼 %s' % (len(wire), [e[0] for e in evs]))

    import time
    t0 = time.time()
    plan = s.frame('%s/proto1.png' % outdir)
    dt = (time.time() - t0) * 1000
    n_spans = sum(len(v) for v in doc_to_spans(doc).values())
    raw = W * H * 4
    print('內容 %d 行 → %d span ／ 首幀 %d B ／ RGBA %d B（%.0f×）'
          % (len(doc['lines']), n_spans, len(wire), raw, raw / len(wire)))
    print('合成 %.0f K 像素 ／ 髒區 %d 塊 ／ 首幀光柵 %.0f ms'
          % (plan['pixels'] / 1e3, len(plan['damage']), dt))

    # 3) 點擊 → 焦點 → 鍵盤
    s.inject_pointer(100, 200)
    ev = proto.decode(s.flush_events())
    print('點擊 (100,200) → %s' % [e for e in ev if e[0] == 'pointer'])

    s.inject_key(ord('j'))
    ev = proto.decode(s.flush_events())
    print('按 j → %s' % [e for e in ev if e[0] == 'key'])

    # 4) 第二幀：只改一行
    wire2 = proto.encode([('spans', 1, 0, 10, [(3, 40, 300, 1)]), ('commit', 1)])
    s.feed(wire2)
    plan = s.frame('%s/proto2.png' % outdir)
    print('改一行：封包 %d B ／ 合成 %.0f K 像素 ／ 幀回呼 %s'
          % (len(wire2), plan['pixels'] / 1e3,
             [e[0] for e in proto.decode(s.flush_events())]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
