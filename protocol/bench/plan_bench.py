#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""plan_bench.py — 同一場景的 Python 版（對照 bench/plan.c）"""
import time

from compositor import Compositor, Rect

N_SURF, N_DMG, FRAMES = 200, 50, 500


def build():
    c = Compositor(1920, 1080)
    seed = 20261110
    for i in range(N_SURF):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        x = (seed >> 8) % 1800
        y = (seed >> 4) % 1000
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        w = 200 + ((seed >> 12) % 600)
        h = 150 + ((seed >> 16) % 400)
        c.add('s%d' % i, x, y, w, h, (40, 40, 50),
              z=i, layer='background' if i == 0 else 'normal',
              opaque=(i % 3) != 0)
    dmg = []
    for i in range(N_DMG):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        dmg.append(Rect((seed >> 8) % 1900, (seed >> 4) % 1000,
                        40 + (seed % 200), 40 + ((seed >> 3) % 160)))
    c.plan()          # 清掉初始 damage
    return c, dmg


def main():
    c, dmg = build()
    total = 0
    t = time.time()
    for _ in range(FRAMES):
        for d in dmg:
            c.damage_rect(d)
        p = c.plan()
        total += p['pixels']
    dt = (time.time() - t) * 1000
    print('Python  %d surfaces / %d damage / %d 幀' % (N_SURF, N_DMG, FRAMES))
    print('  總計 %8.1f ms ｜ 每幀 %6.3f ms ｜ 平均像素 %.0f K'
          % (dt, dt / FRAMES, total / FRAMES / 1000.0))


if __name__ == '__main__':
    main()
