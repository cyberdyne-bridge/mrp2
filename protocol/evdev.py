#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""evdev.py — Linux 輸入裝置 → MRP2 事件

不做 libinput、不做 XKB：直接讀 /dev/input/event*，把
EV_KEY / EV_REL / EV_SYN 翻成 MRP2 的 KEY / POINTER。

  python3 evdev.py --dump /dev/input/event3     # 看原始事件
  python3 evdev.py --seat /dev/input/event3 /dev/input/event4

iSH 沒有 /dev/input，所以這裡只驗到「解析 + 分發」，
真裝置要等 Artix。
"""
import os
import select
import struct
import sys

# struct input_event：64 位是 24 bytes（timeval 兩個 long）
EVENT_FMT = 'llHHi'
EVENT_SIZE = struct.calcsize(EVENT_FMT)

EV_SYN, EV_KEY, EV_REL, EV_ABS, EV_MSC = 0x00, 0x01, 0x02, 0x03, 0x04
REL_X, REL_Y, REL_WHEEL = 0x00, 0x01, 0x08
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 0x110, 0x111, 0x112

XK = {
    'Return': 0xFF0D, 'Escape': 0xFF1B, 'BackSpace': 0xFF08, 'Tab': 0xFF09,
    'space': 0x20, 'Up': 0xFF52, 'Down': 0xFF54, 'Left': 0xFF51, 'Right': 0xFF53,
    'Home': 0xFF50, 'End': 0xFF57, 'Page_Up': 0xFF55, 'Page_Down': 0xFF56,
    'Delete': 0xFFFF, 'Shift_L': 0xFFE1, 'Control_L': 0xFFE3, 'Alt_L': 0xFFE9,
}

# Linux input key code → 名字（只列我們會用到的）
KEYMAP = {
    1: 'Escape', 14: 'BackSpace', 15: 'Tab', 28: 'Return', 57: 'space',
    16: 'q', 17: 'w', 18: 'e', 19: 'r', 20: 't', 21: 'y', 22: 'u', 23: 'i',
    24: 'o', 25: 'p', 30: 'a', 31: 's', 32: 'd', 33: 'f', 34: 'g', 35: 'h',
    36: 'j', 37: 'k', 38: 'l', 44: 'z', 45: 'x', 46: 'c', 47: 'v', 48: 'b',
    49: 'n', 50: 'm', 42: 'Shift_L', 29: 'Control_L', 56: 'Alt_L',
    103: 'Up', 105: 'Left', 106: 'Right', 108: 'Down',
    102: 'Home', 107: 'End', 104: 'Page_Up', 109: 'Page_Down',
}
MODS = {'Shift_L': 1, 'Control_L': 4, 'Alt_L': 8}


def keysym_for(code):
    name = KEYMAP.get(code)
    if not name:
        return None, None
    if len(name) == 1 and name.isalnum():
        return ord(name), name
    return XK.get(name), name


def parse(raw):
    """24 bytes → (sec, usec, type, code, value)"""
    return struct.unpack(EVENT_FMT, raw)


def pack(sec, usec, typ, code, value):
    return struct.pack(EVENT_FMT, sec, usec, typ, code, value)


class Seat:
    """把裝置事件翻成伺服器呼叫；指標用相對位移累積。"""

    def __init__(self, server, x=None, y=None):
        self.srv = server
        self.x = server.w // 2 if x is None else x
        self.y = server.h // 2 if y is None else y
        self.mods = 0
        self.wheel = 0
        self.buttons = {}
        self.dirty = False          # 有待送的位移（等 SYN_REPORT 再送）

    def clamp(self):
        self.x = max(0, min(self.srv.w - 1, self.x))
        self.y = max(0, min(self.srv.h - 1, self.y))

    def handle(self, typ, code, value):
        if typ == EV_SYN:
            # 內核把一次動作的多個 REL 用 SYN_REPORT 收尾 → 只送一個事件
            if code == 0 and self.dirty:
                self.srv.inject_pointer_move(self.x, self.y)
                self.dirty = False
            return
        if typ == EV_REL:
            if code == REL_X:
                self.x += value
                self.clamp()
                self.dirty = True
            elif code == REL_Y:
                self.y += value
                self.clamp()
                self.dirty = True
            elif code == REL_WHEEL:
                self.wheel += value
            return
        if typ != EV_KEY:
            return

        if code in (BTN_LEFT, BTN_RIGHT, BTN_MIDDLE):
            btn = {BTN_LEFT: 1, BTN_RIGHT: 2, BTN_MIDDLE: 3}[code]
            self.buttons[btn] = bool(value)
            if value == 1:
                self.srv.inject_pointer(self.x, self.y, btn, 1)
            elif value == 0:
                self.srv.inject_pointer(self.x, self.y, btn, 0)
            return

        ks, name = keysym_for(code)
        if name is None:
            return                      # 未知鍵：忽略，不炸
        if name in MODS:
            if value == 1:
                self.mods |= MODS[name]
            elif value == 0:
                self.mods &= ~MODS[name]
        if value in (1, 2):             # 按下／重複
            self.srv.inject_key(ks, 1, self.mods)
        elif value == 0:
            self.srv.inject_key(ks, 0, self.mods)

    def feed(self, data):
        """餵一整塊 bytes（可能含多個事件）。"""
        n = 0
        for i in range(0, len(data) - EVENT_SIZE + 1, EVENT_SIZE):
            _, _, typ, code, value = parse(data[i:i + EVENT_SIZE])
            self.handle(typ, code, value)
            n += 1
        return n


def drain(fd, seat, chunk=EVENT_SIZE * 64):
    data = os.read(fd, chunk)
    return seat.feed(data)


def main(argv=None):
    argv = argv or sys.argv[1:]
    if '--dump' in argv:
        path = argv[argv.index('--dump') + 1]
        fd = os.open(path, os.O_RDONLY)
        print('讀 %s（24 bytes/event）' % path)
        while True:
            data = os.read(fd, EVENT_SIZE * 64)
            for i in range(0, len(data), EVENT_SIZE):
                s, us, t, c, v = parse(data[i:i + EVENT_SIZE])
                print('%d.%06d type=%d code=%d value=%d' % (s, us, t, c, v))
    if '--seat' in argv:
        from server import Server
        from display import MockDisplay
        paths = [a for a in argv[argv.index('--seat') + 1:] if a.startswith('/dev/')]
        srv = Server(1920, 1080, MockDisplay(1920, 1080))
        seat = Seat(srv)
        fds = [os.open(p, os.O_RDONLY | os.O_NONBLOCK) for p in paths]
        print('接上 %d 個裝置，等待事件…' % len(fds))
        while True:
            r, _, _ = select.select(fds, [], [])
            for fd in r:
                try:
                    drain(fd, seat)
                except OSError as e:
                    print('讀取失敗：%s' % e)
    if not argv:
        print(__doc__)
    return 0


if __name__ == '__main__':
    sys.exit(main())
