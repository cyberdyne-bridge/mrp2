#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""compositor_sim.py — 桌面合成層的「省像素」算術模型（不畫圖，只算面積）

驗的是三種策略在同一組場景下「實際合成多少像素」：
  naive  ：每幀整屏重畫
  damage ：只重畫髒區（wl_surface.damage）
  d+opq  ：髒區再扣掉被上層不透明 surface 蓋住的部分（set_opaque_region）

場景：1920×1080，底圖 + 三個視窗
"""
W, H = 1920, 1080
SCREEN = W * H


class Rect:
    __slots__ = ('x', 'y', 'w', 'h')

    def __init__(self, x, y, w, h):
        self.x, self.y, self.w, self.h = x, y, w, h

    def area(self):
        return max(0, self.w) * max(0, self.h)

    def inter(self, o):
        x0, y0 = max(self.x, o.x), max(self.y, o.y)
        x1 = min(self.x + self.w, o.x + o.w)
        y1 = min(self.y + self.h, o.y + o.h)
        return Rect(x0, y0, x1 - x0, y1 - y0) if x1 > x0 and y1 > y0 else None

    def union(self, o):
        if not o:
            return self
        x0, y0 = min(self.x, o.x), min(self.y, o.y)
        x1 = max(self.x + self.w, o.x + o.w)
        y1 = max(self.y + self.h, o.y + o.h)
        return Rect(x0, y0, x1 - x0, y1 - y0)


def merge(rects):
    out = []
    for r in rects:
        if r.area() <= 0:
            continue
        if out:
            m = out[-1].union(r)
            if m.area() <= out[-1].area() + r.area():   # 重疊就併
                out[-1] = m
                continue
        out.append(r)
    return out


def pixels(damage, occluders):
    """髒區面積，扣掉被不透明視窗蓋住的部分。"""
    total = 0
    for d in merge(damage):
        area = d.area()
        for o in occluders:
            cut = d.inter(o)
            if cut:
                area -= cut.area()
        total += max(0, area)
    return total


def scenario(name, damage, occluders=()):
    naive = SCREEN
    dm = pixels(damage, ())
    do = pixels(damage, occluders)
    print('%-14s 整屏 %6.0fK ｜ 髒區 %7.1fK (%5.1f%%) ｜ 剔除後 %7.1fK (%5.1f%%)'
          % (name, naive / 1e3, dm / 1e3, 100 * dm / naive, do / 1e3, 100 * do / naive))


def main():
    win_a = Rect(100, 80, 900, 700)
    win_b = Rect(1050, 120, 700, 500)
    win_c = Rect(700, 600, 600, 400)      # 疊在 a 上面，不透明

    print('螢幕 %dx%d = %.2f M 像素\n' % (W, H, SCREEN / 1e6))

    scenario('捲動一屏', [Rect(100, 80, 900, 700)])
    scenario('拖動視窗', [win_a, Rect(140, 110, 900, 700)])
    scenario('游標閃爍', [Rect(960, 540, 20, 20)])
    scenario('全屏播放', [Rect(0, 0, W, H)])
    print()
    scenario('捲動（有不透明上層）', [Rect(100, 80, 900, 700)], [win_c, win_b])
    scenario('全屏播放（直掃）', [Rect(0, 0, W, H)], [Rect(0, 0, W, H)])


if __name__ == '__main__':
    main()
