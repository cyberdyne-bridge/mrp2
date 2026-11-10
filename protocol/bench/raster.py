#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""raster.py — 同一份工作負載的 Python 版（對照 C / Node）"""
import time

W, H, LINES, COLS = 1232, 1488, 44, 72
buf = bytearray(W * H * 3)


def clear():
    buf[:] = b'\x0b' * (W * H * 3)


def solid():
    for ln in range(LINES):
        y = 20 + ln * 16
        for c in range(COLS):
            x = 20 + c * 8
            for dy in range(12):
                off = ((y + dy) * W + x) * 3
                buf[off:off + 7 * 3] = b'\xe8' * (7 * 3)


def ink():
    seed = 12345
    for ln in range(LINES):
        y = 20 + ln * 16
        for c in range(COLS):
            x = 20 + c * 8
            for dy in range(12):
                seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
                if (seed >> 16) % 100 < 22:
                    x0 = (seed >> 5) % 6
                    off = ((y + dy) * W + x + x0) * 3
                    buf[off:off + 3 * 3] = b'\xe8' * 9


if __name__ == '__main__':
    print('Python  %dx%d' % (W, H))
    for name, fn in (('clear', clear), ('solid', solid), ('ink', ink)):
        best = 1e9
        for _ in range(3):
            t = time.time()
            fn()
            best = min(best, (time.time() - t) * 1000)
        print('  %-6s %7.2f ms' % (name, best))
