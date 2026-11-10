#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_proto.py — MRP2（含 v1 兼容）的單元測試"""
import struct
import sys

import proto
from compositor import Rect, Scheduler
from display import MockDisplay
from server import Server

ok = total = 0


def check(name, cond, extra=''):
    global ok, total
    total += 1
    print('  %-40s %s %s' % (name, 'PASS' if cond else 'FAIL', extra))
    ok += 1 if cond else 0


def main():
    print('1. v2 編解碼往返')
    ops = [
        ('hello', 2, proto.CAP_SPANS | proto.CAP_FRAME_CALLBACK),
        ('surf', 1, 10, 20, 300, 200, 3, 1, 0b101, 1, 0),
        ('spans', 1, 0, 2, [(3, 8, 120, 0), (4, 8, 160, 1)]),
        ('damage', 1, 0, 0, 50, 20),
        ('commit', 1),
        ('unmap', 1),
    ]
    wire = proto.encode(ops)
    back = proto.decode(wire)
    check('op 數一致', len(back) == len(ops))
    check('SURF 含 role/parent', back[1] == ops[1], back[1])
    check('SPANS 內容一致', back[2][4] == ops[2][4])
    check('magic = MRP2', wire[:4] == b'MRP2')

    print('2. v1 封包仍可解（舊 client 不會斷）')
    hdr = struct.pack('<4sBBBI', b'MRP1', 1, 0, 0, 1)
    body = struct.pack('<BH', 1, 14) + struct.pack('<HhhHHhBB', 7, 10, 20, 100, 50, 0, 1, 1)
    v1 = proto.decode(hdr + body)
    check('v1 SURF 補上 role/parent', v1[0] == ('surf', 7, 10, 20, 100, 50, 0, 1, 1, 1, 0), v1[0])

    print('3. 握手')
    d = MockDisplay(320, 200)
    s = Server(320, 200, d, refresh=60)
    s.feed(proto.encode([('hello', 2, proto.CAP_SPANS | proto.CAP_FRAME_CALLBACK)]))
    ev = proto.decode(s.flush_events())[0]
    check('hello_ack 回 proto=2', ev[0] == 'hello_ack' and ev[1] == 2)
    check('帶輸出尺寸與刷新率', (ev[4], ev[5], ev[6]) == (320, 200, 60), ev[4:])

    print('4. 幀回呼只在協商過才送')
    s2 = Server(320, 200, MockDisplay(320, 200))
    s2.feed(proto.encode([('surf', 1, 0, 0, 100, 100, 0, 1, 1, 1, 0), ('commit', 1)]))
    check('沒 CAP 就沒有 frame', not [e for e in s2.out if e[0] == 'frame'])
    s.feed(proto.encode([('surf', 1, 0, 0, 100, 100, 0, 1, 1, 1, 0), ('commit', 1)]))
    evs = proto.decode(s.flush_events())
    check('有 CAP 才有 frame', [e[0] for e in evs] == ['frame'], evs)

    print('5. 輸入分發')
    s = Server(320, 200, MockDisplay(320, 200))
    s.feed(proto.encode([('surf', 5, 10, 10, 100, 80, 0, 1, 0b101, 1, 0), ('commit', 5)]))
    s.flush_events()
    s.inject_pointer(50, 50)
    ev = proto.decode(s.flush_events())
    check('pointer 帶 surface id', ev[0][1] == 5, ev[0])
    s.inject_key(ord('k'))
    ev = proto.decode(s.flush_events())
    check('key 送給焦點', ev[0][1] == 5 and ev[0][3] == ord('k'), ev[0])
    check('serial 遞增', ev[0][2] > 0, ev[0][2])

    print('6. 髒區與 commit 語義')
    s = Server(320, 200, MockDisplay(320, 200))
    s.feed(proto.encode([('surf', 1, 0, 0, 200, 160, 0, 1, 1, 1, 0)]))
    s.comp.plan()
    s.feed(proto.encode([('spans', 1, 0, 3, [(2, 0, 40, 1)])]))
    dmg = s.comp.pending[0]
    check('只髒 tile 那一條', dmg.h == 16 and dmg.y == 22 + 48, dmg)
    s.comp.pending = []
    s.feed(proto.encode([('commit', 1)]))
    check('commit 不膨脹髒區', s.comp.pending == [])

    print('7. 未知 op 要跳過（向前兼容）')
    hdr = struct.pack('<4sBBBI', b'MRP2', 2, 0, 0, 2)
    unknown = struct.pack('<BH', 0x7E, 3) + b'abc'
    commit = struct.pack('<BH', 4, 2) + struct.pack('<H', 1)
    got = proto.decode(hdr + unknown + commit)
    check('只剩已知 op', got == [('commit', 1)], got)

    print('8. 位元組經濟')
    raw = 960 * 600 * 4
    check('封包 < 整幀 /100', len(wire) < raw / 100, '%d vs %d' % (len(wire), raw))

    print('9. 幀節流')
    s = Server(320, 200, MockDisplay(320, 200))
    s.feed(proto.encode([('surf', 1, 0, 0, 320, 200, 0, 1, 1, 1, 0)]))
    sch = Scheduler(60)
    frames = 0
    for t in range(201):
        s.feed(proto.encode([('spans', 1, 0, 0, [(t % 16, 0, 100, 0)])]))
        if sch.poll(float(t), s.comp):
            frames += 1
    check('200 ms → 13 幀', frames == 13, frames)

    print('\n%d/%d 通過' % (ok, total))
    return 0 if ok == total else 1


if __name__ == '__main__':
    sys.exit(main())
