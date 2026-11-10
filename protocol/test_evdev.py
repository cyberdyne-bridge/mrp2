#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_evdev.py — 輸入層（解析 / 鍵碼 / 分發）的單元測試"""
import sys

import evdev
import proto
from display import MockDisplay
from server import Server

ok = total = 0


def check(name, cond, extra=''):
    global ok, total
    total += 1
    print('  %-40s %s %s' % (name, 'PASS' if cond else 'FAIL', extra))
    ok += 1 if cond else 0


def build():
    s = Server(1920, 1080, MockDisplay(1920, 1080))
    s.feed(proto.encode([('surf', 1, 100, 100, 800, 600, 0, 1, 0b101, 1, 0),
                         ('commit', 1)]))
    s.flush_events()
    return s, evdev.Seat(s, x=200, y=200)


def main():
    print('1. struct 往返')
    raw = evdev.pack(1, 2, evdev.EV_KEY, 36, 1)
    check('24 bytes', len(raw) == 24)
    check('欄位正確', evdev.parse(raw) == (1, 2, evdev.EV_KEY, 36, 1))

    print('2. 鍵碼 → keysym')
    check('j → 0x6a', evdev.keysym_for(36) == (0x6A, 'j'))
    check('Enter → 0xFF0D', evdev.keysym_for(28) == (0xFF0D, 'Return'))
    check('未知鍵 → None', evdev.keysym_for(999) == (None, None))

    print('3. 指標移動')
    s, seat = build()
    seat.feed(evdev.pack(0, 0, evdev.EV_REL, evdev.REL_X, 40) +
              evdev.pack(0, 0, evdev.EV_REL, evdev.REL_Y, 30) +
              evdev.pack(0, 0, evdev.EV_SYN, 0, 0))
    ev = [e for e in proto.decode(s.flush_events()) if e[0] == 'pointer']
    check('X/Y 合併成一個事件', len(ev) == 1, len(ev))
    check('座標正確', (ev[-1][2], ev[-1][3]) == (240, 230), ev[-1])

    print('4. 邊界夾住')
    s, seat = build()
    seat.feed(evdev.pack(0, 0, evdev.EV_REL, evdev.REL_X, -9999) +
              evdev.pack(0, 0, evdev.EV_SYN, 0, 0))
    ev = proto.decode(s.flush_events())
    check('x 夾到 0', ev[-1][2] == 0, ev[-1])
    seat.feed(evdev.pack(0, 0, evdev.EV_REL, evdev.REL_Y, 99999) +
              evdev.pack(0, 0, evdev.EV_SYN, 0, 0))
    ev = proto.decode(s.flush_events())
    check('y 夾到 h-1', ev[-1][3] == 1079, ev[-1])

    print('5. 點擊 → 焦點')
    s, seat = build()
    seat.feed(evdev.pack(0, 0, evdev.EV_KEY, evdev.BTN_LEFT, 1))
    check('拿到焦點', s.comp.focus == 1, s.comp.focus)
    s.flush_events()
    seat.x, seat.y = 5, 5                         # 移到空白處再放開
    seat.feed(evdev.pack(0, 0, evdev.EV_KEY, evdev.BTN_LEFT, 0))
    check('放開不改焦點', s.comp.focus == 1, s.comp.focus)

    print('6. 鍵盤送給焦點')
    s, seat = build()
    seat.handle(evdev.EV_KEY, evdev.BTN_LEFT, 1)
    s.flush_events()
    seat.feed(evdev.pack(0, 0, evdev.EV_KEY, 36, 1))
    ev = proto.decode(s.flush_events())
    check('KEY 事件', ev[0][0] == 'key' and ev[0][3] == 0x6A, ev[0])
    check('surface 正確', ev[0][1] == 1, ev[0])

    print('7. Shift 修飾鍵')
    s, seat = build()
    seat.handle(evdev.EV_KEY, evdev.BTN_LEFT, 1)
    s.flush_events()
    seat.handle(evdev.EV_KEY, 42, 1)              # Shift 按下
    seat.handle(evdev.EV_KEY, 36, 1)              # j
    ev = proto.decode(s.flush_events())
    check('mods 帶 Shift', ev[-1][5] & 1 == 1, ev[-1])

    print('8. 未知鍵不炸')
    s, seat = build()
    n = seat.feed(evdev.pack(0, 0, evdev.EV_KEY, 999, 1))
    check('回傳事件數 1', n == 1)
    check('沒有產生事件', s.out == [], s.out)

    print('9. 一次餵多個事件')
    s, seat = build()
    blob = (evdev.pack(0, 0, evdev.EV_REL, evdev.REL_X, 5) +
            evdev.pack(0, 0, evdev.EV_REL, evdev.REL_X, 5) +
            evdev.pack(0, 0, evdev.EV_SYN, 0, 0))
    check('解析 3 個', seat.feed(blob) == 3)
    ev = [e for e in proto.decode(s.flush_events()) if e[0] == 'pointer']
    check('x 累積到 210', ev[-1][2] == 210, ev[-1])

    print('\n%d/%d 通過' % (ok, total))
    return 0 if ok == total else 1


if __name__ == '__main__':
    sys.exit(main())
