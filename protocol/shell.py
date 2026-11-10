#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""shell.py — 系統入口：輸入 → 協議 → 合成 → 60 Hz → 顯示

這是把前面所有零件接起來的那條線：
  evdev（真裝置）或 --scripted（假事件）
        → Server（MRP2）
        → Compositor.plan + 光柵
        → Display.flip（Mock；真機換 DRM）

  python3 shell.py --mock --frames 60            # iSH 可跑
  python3 shell.py --size 1920x1080 --input /dev/input/event3 /dev/input/event4
"""
import argparse
import os
import select
import sys
import time

import evdev
import proto
from compositor import Rect, Scheduler
from server import Server


def build_scene(srv, W, H, doc_path=None):
    import json
    ops = [('surf', 90, 0, 0, W, H, 0, 0, 0b001, 0, 0)]      # 桌布
    ops.append(('surf', 2, W - 380, 40, 360, 220, 2, 1, 0b101, 1, 0))
    ops.append(('commit', 2))
    srv.feed(proto.encode(ops))
    srv.feed(proto.encode([('hello', 2, proto.CAP_SPANS | proto.CAP_FRAME_CALLBACK)]))
    srv.flush_events()
    if doc_path and os.path.exists(doc_path):
        import demo_proto as dp
        doc = json.load(open(doc_path, encoding='utf-8'))
        srv.feed(proto.encode(dp.build_ops(doc, sid=1, x=40, y=60, w=W // 2, h=H - 120)))
        srv.flush_events()


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--size', default='1920x1080')
    ap.add_argument('--fps', type=int, default=60)
    ap.add_argument('--frames', type=int, default=0, help='0 = 無限')
    ap.add_argument('--input', nargs='*', default=[], help='/dev/input/event*')
    ap.add_argument('--mock', action='store_true', help='不用真裝置，自己動游標')
    ap.add_argument('--scripted', action='store_true', help='跑一段固定輸入腳本')
    ap.add_argument('--doc', default='out/doc.json')
    ap.add_argument('--out', default='', help='存最後一幀 PNG')
    args = ap.parse_args(argv)

    W, H = [int(v) for v in args.size.split('x')]
    srv = Server(W, H, refresh=args.fps)
    build_scene(srv, W, H, args.doc)
    seat = evdev.Seat(srv)
    srv.frame()                                     # 首幀

    fds = []
    for p in args.input:
        fds.append(os.open(p, os.O_RDONLY | os.O_NONBLOCK))

    sch = Scheduler(args.fps)
    t0 = time.monotonic()
    frames = 0
    script = [('move', 600, 400), ('click', 0, 0), ('key', ord('j'), 0),
              ('move', 120, 120), ('move', 700, 300)]
    si = 0

    while args.frames == 0 or frames < args.frames:
        now = (time.monotonic() - t0) * 1000.0

        if fds:
            r, _, _ = select.select(fds, [], [], 0.001)
            for fd in r:
                evdev.drain(fd, seat)
        elif args.mock or args.scripted:
            if si < len(script):
                kind, a, b = script[si]
                if kind == 'move':
                    seat.x, seat.y = a, b
                    seat.dirty = True
                    seat.handle(evdev.EV_SYN, 0, 0)
                elif kind == 'click':
                    seat.handle(evdev.EV_KEY, evdev.BTN_LEFT, 1)
                    seat.handle(evdev.EV_KEY, evdev.BTN_LEFT, 0)
                elif kind == 'key':
                    seat.handle(evdev.EV_KEY, 36, 1)
                si += 1
            else:
                # 腳本跑完後讓游標慢慢飄，模擬持續輸入
                seat.x = (seat.x + 7) % W
                seat.y = (seat.y + 3) % H
                seat.dirty = True
                seat.handle(evdev.EV_SYN, 0, 0)
            # 游標移動要讓合成器有髒區，否則不會出幀（就是剛剛卡死的原因）
            srv.comp.damage_rect(Rect(seat.x - 12, seat.y - 12, 24, 24))

        if sch.due(now) and srv.comp.pending:
            srv.frame(args.out if (args.frames and frames == args.frames - 1) else None)
            frames += 1
        else:
            time.sleep(0.001)

    dt = (time.monotonic() - t0) * 1000.0
    print('%d 幀 / %.0f ms → %.1f fps（目標 %d）' % (frames, dt, frames * 1000.0 / dt, args.fps))
    print('協議 op %d ／ span %d ／ 翻頁 %d' % (srv.ops_seen, srv.spans_seen, srv.display.flips))
    if args.out:
        print('最後一幀：%s' % args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
