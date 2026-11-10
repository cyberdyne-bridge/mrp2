#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""compositor.py — 桌面合成器的「決策核心」（純邏輯，可測，iSH 可跑）

這一層不碰 Wayland，只回答合成器每幀要決定的事：
  1. 有沒有東西要畫（沒有就跳幀）／幀節流
  2. 該用 direct scanout 嗎（全屏不透明 + 單一 surface）
  3. 每個髒區裡，哪些 surface 真的可見（不透明剔除、layer 次序）
  4. 點擊命中誰、鍵盤焦點給誰（focus stack / alt-tab）
  5. 總共要合成多少像素

對應真實協議：wl_surface.damage / set_opaque_region / direct scanout /
wl_seat 焦點 / layer-shell。

  python3 compositor.py --demo out
"""
import sys

import render
from render import SoftRenderer

BG = (12, 12, 16)
LAYERS = {'background': 0, 'normal': 1, 'top': 2, 'overlay': 3}


class Rect:
    __slots__ = ('x', 'y', 'w', 'h')

    def __init__(self, x, y, w, h):
        self.x, self.y, self.w, self.h = int(x), int(y), int(w), int(h)

    def area(self):
        return max(0, self.w) * max(0, self.h)

    def empty(self):
        return self.area() == 0

    def has(self, px, py):
        return self.x <= px < self.x + self.w and self.y <= py < self.y + self.h

    def intersect(self, o):
        x0, y0 = max(self.x, o.x), max(self.y, o.y)
        x1, y1 = min(self.x + self.w, o.x + o.w), min(self.y + self.h, o.y + o.h)
        return Rect(x0, y0, x1 - x0, y1 - y0) if x1 > x0 and y1 > y0 else None

    def union(self, o):
        if o is None:
            return self
        x0, y0 = min(self.x, o.x), min(self.y, o.y)
        x1, y1 = max(self.x + self.w, o.x + o.w), max(self.y + self.h, o.y + o.h)
        return Rect(x0, y0, x1 - x0, y1 - y0)

    def contains(self, o):
        return (self.x <= o.x and self.y <= o.y and
                self.x + self.w >= o.x + o.w and self.y + self.h >= o.y + o.h)

    def __repr__(self):
        return 'Rect(%d,%d,%d,%d)' % (self.x, self.y, self.w, self.h)


def subtract(rect, cut):
    inter = rect.intersect(cut)
    if inter is None:
        return [rect]
    out = []
    if inter.y > rect.y:
        out.append(Rect(rect.x, rect.y, rect.w, inter.y - rect.y))
    if inter.y + inter.h < rect.y + rect.h:
        out.append(Rect(rect.x, inter.y + inter.h, rect.w,
                        rect.y + rect.h - (inter.y + inter.h)))
    if inter.x > rect.x:
        out.append(Rect(rect.x, inter.y, inter.x - rect.x, inter.h))
    if inter.x + inter.w < rect.x + rect.w:
        out.append(Rect(inter.x + inter.w, inter.y,
                        rect.x + rect.w - (inter.x + inter.w), inter.h))
    return [r for r in out if not r.empty()]


def merge(rects):
    out = []
    for r in sorted(rects, key=lambda r: (r.y, r.x)):
        for i, o in enumerate(out):
            adjacent = (o.x == r.x and o.w == r.w and
                        (o.y + o.h == r.y or r.y + r.h == o.y))
            if o.intersect(r) or adjacent:
                out[i] = o.union(r)
                break
        else:
            out.append(Rect(r.x, r.y, r.w, r.h))
    return out


class Surface:
    def __init__(self, sid, title, x, y, w, h, color,
                 z=0, layer='normal', opaque=True,
                 input=True, keyboard=True, content=None):
        self.id = sid
        self.title = title
        self.x, self.y, self.w, self.h = x, y, w, h
        self.color = color
        self.z = z
        self.layer = layer
        self.opaque = opaque
        self.visible = True
        self.input = input            # 要不要參與點擊命中
        self.keyboard = keyboard      # 能不能拿鍵盤焦點
        self.content = content        # display list 的 lines
        self.scroll = 0

    def rect(self):
        return Rect(self.x, self.y, self.w, self.h)

    def rank(self):
        return (LAYERS.get(self.layer, 1), self.z, self.id)


class Compositor:
    def __init__(self, w, h, bg=BG):
        self.w, self.h = w, h
        self.bg = bg
        self.surfaces = {}
        self.seq = 0
        self.pending = []
        self.frames = 0
        self.skipped = 0
        self.focus = None
        self.focus_stack = []      # MRU
        self.events = []           # 輸入事件紀錄（測驗用）

    # ── 場景 ──
    def add(self, title, x, y, w, h, color, z=0, layer='normal',
            opaque=True, input=True, keyboard=True, content=None):
        self.seq += 1
        s = Surface(self.seq, title, x, y, w, h, color, z, layer,
                    opaque, input, keyboard, content)
        self.surfaces[s.id] = s
        self.damage_rect(s.rect())
        return s

    def move(self, sid, x, y):
        s = self.surfaces[sid]
        old = s.rect()
        s.x, s.y = x, y
        self.damage_rect(old)
        self.damage_rect(s.rect())

    def resize(self, sid, w, h):
        s = self.surfaces[sid]
        old = s.rect()
        s.w, s.h = w, h
        self.damage_rect(old)
        self.damage_rect(s.rect())

    def set_visible(self, sid, vis):
        s = self.surfaces[sid]
        s.visible = vis
        if not vis and self.focus == sid:
            self.set_focus(None)
        self.damage_rect(s.rect())

    def damage(self, sid, rect=None):
        s = self.surfaces[sid]
        r = s.rect() if rect is None else Rect(s.x + rect.x, s.y + rect.y, rect.w, rect.h)
        self.damage_rect(r)

    def damage_rect(self, r):
        r = r.intersect(Rect(0, 0, self.w, self.h))
        if r:
            self.pending.append(r)

    # ── 次序與命中 ──
    def stack(self):
        return sorted(self.surfaces.values(), key=lambda s: s.rank())

    def hit(self, x, y, layers=None):
        """回最上層可命中的 surface。"""
        for s in reversed(self.stack()):
            if not s.visible or not s.input:
                continue
            if layers and s.layer not in layers:
                continue
            if s.rect().has(x, y):
                return s
        return None

    # ── 焦點 ──
    def set_focus(self, sid):
        self.focus = sid
        if sid is not None:
            self.focus_stack = [sid] + [f for f in self.focus_stack if f != sid]
        self.events.append(('focus', sid))

    def focus_cycle(self, step=1):
        """alt-tab：MRU 優先，其餘視窗按 z 由上往下補。"""
        vis = [s for s in self.stack() if s.visible and s.keyboard and s.input]
        ids = {s.id for s in vis}
        order = [f for f in self.focus_stack if f in ids]
        order += [s.id for s in reversed(vis) if s.id not in order]
        if not order:
            return None
        i = order.index(self.focus) if self.focus in order else -1
        self.set_focus(order[(i + step) % len(order)])
        return self.focus

    def pointer_button(self, x, y, button=1):
        s = self.hit(x, y)
        if s is None:
            self.events.append(('click', None, x, y))
            self.set_focus(None)
            return None
        self.events.append(('click', s.id, x, y))
        if s.keyboard:
            self.set_focus(s.id)
        return s.id

    def key(self, k):
        """鍵盤事件只送給焦點視窗。"""
        if self.focus is None:
            self.events.append(('key', None, k))
            return None
        self.events.append(('key', self.focus, k))
        return self.focus

    # ── 決策 ──
    def plan(self):
        screen = Rect(0, 0, self.w, self.h)
        if not self.pending:
            self.skipped += 1
            return {'need_frame': False, 'scanout': None, 'ops': [],
                    'damage': [], 'pixels': 0, 'bg': 0}

        vis = [s for s in self.stack() if s.visible]
        top = vis[-1] if vis else None
        can_scan = (top is not None and top.opaque
                    and top.layer in ('normal', 'background')
                    and top.rect().contains(screen)
                    and not any(s.layer in ('top', 'overlay') for s in vis))
        if can_scan:
            self.pending = []
            self.frames += 1
            return {'need_frame': True, 'scanout': top.id, 'ops': [],
                    'damage': [screen], 'pixels': 0, 'bg': 0}

        dmg = merge(self.pending)
        self.pending = []
        ops = []
        for d in dmg:
            self._collect(d, ops)
        ops.sort(key=lambda o: self.surfaces[o['surface']].rank())
        self.frames += 1
        return {'need_frame': True, 'scanout': None, 'ops': ops,
                'damage': dmg,
                'pixels': sum(o['rect'].area() for o in ops),
                'bg': sum(d.area() for d in dmg)}

    def _collect(self, rect, ops):
        pieces = [rect]
        for s in reversed(self.stack()):
            if not s.visible:
                continue
            nxt = []
            for p in pieces:
                inter = p.intersect(s.rect())
                if inter is None:
                    nxt.append(p)
                    continue
                ops.append({'surface': s.id, 'rect': inter})
                if s.opaque:
                    nxt.extend(subtract(p, inter))
                else:
                    nxt.append(p)
            pieces = nxt
            if not pieces:
                break


# ─────────────── 幀節流 ───────────────
class Scheduler:
    """60 Hz 格線；同一格內的多次 damage 合併成一幀。"""

    def __init__(self, hz=60.0):
        self.interval = 1000.0 / hz
        self.next_due = 0.0

    EPS = 1e-6

    def due(self, now_ms):
        """只回答「現在該不該出幀」，不消費髒區。"""
        if now_ms + self.EPS < self.next_due:
            return False
        while self.next_due <= now_ms + self.EPS:
            self.next_due += self.interval
        return True

    def poll(self, now_ms, comp):
        if not comp.pending:
            return None
        if now_ms + self.EPS < self.next_due:
            return None
        while self.next_due <= now_ms + self.EPS:   # 對齊格線，不累積漂移
            self.next_due += self.interval
        return comp.plan()


# ─────────────── 渲染（示範用） ───────────────
def _light(c, d):
    return tuple(min(255, v + d) for v in c)


def _draw_lines(r, s, clip, lh=16, cw=8):
    """把 surface 的 display list 畫進 clip 區域（超出就丟）。"""
    if not s.content:
        return
    y = s.y + 18 - s.scroll * lh
    for ln in s.content:
        if y + lh <= clip.y:
            y += lh
            continue
        if y >= clip.y + clip.h:
            break
        x = s.x + 8
        kind = ln.get('k', 'p')
        for seg in ln.get('s', []):
            color = render.DIM if seg.get('n') else render._color(kind, seg)
            for ch in seg['t']:
                cy = max(y, clip.y)
                ch_h = min(y + lh, clip.y + clip.h) - cy
                if ch_h > 0 and clip.x <= x < clip.x + clip.w:
                    render._cell(r, x, cy, ch, color, cw, lh, 1)
                x += render.dw(ch) * cw
        y += lh


def raster(comp, plan, path, draw_chrome=True):
    r = SoftRenderer()
    r.begin(comp.w, comp.h)
    if not plan['need_frame']:
        return r
    for d in plan['damage']:
        r.rect(d.x, d.y, d.w, d.h, comp.bg)

    if plan['scanout']:
        s = comp.surfaces[plan['scanout']]
        r.rect(s.x, s.y, s.w, s.h, s.color)
        return r

    for op in plan['ops']:
        s = comp.surfaces[op['surface']]
        d = op['rect']
        r.rect(d.x, d.y, d.w, d.h, s.color)
        if draw_chrome and s.layer != 'background':
            bar = Rect(s.x, s.y, s.w, 22).intersect(d)
            if bar:
                r.rect(bar.x, bar.y, bar.w, bar.h, _light(s.color, 22))
                if s.title:
                    r.text(max(bar.x + 6, s.x + 6), bar.y + 5,
                           s.title[:max(1, (d.w - 10) // 8)], (240, 240, 240), 1)
        _draw_lines(r, s, d)
    r.flush(path)
    return r


def demo(outdir='out'):
    import json
    import os
    os.makedirs(outdir, exist_ok=True)
    c = Compositor(960, 600)

    # 背景 + 面板（layer）
    c.add('wallpaper', 0, 0, 960, 600, (18, 18, 26), layer='background', input=False)
    panel = c.add('panel', 0, 0, 960, 28, (34, 36, 46), layer='top',
                  keyboard=False)
    a = c.add('reader', 40, 60, 520, 380, (58, 64, 88), z=1)
    b = c.add('bilibili', 380, 220, 480, 300, (86, 42, 52), z=2)
    cur = c.add('cursor', 700, 320, 10, 10, (230, 230, 230), z=9, keyboard=False)

    # 把 reader 的頁面塞進視窗
    try:
        doc = json.load(open('%s/doc.json' % outdir, encoding='utf-8'))
        a.content = doc['lines']
    except Exception:
        pass

    c.pending = [Rect(0, 0, c.w, c.h)]
    p = c.plan()
    raster(c, p, '%s/comp1.png' % outdir)
    print('第 1 幀：合成 %.0f K 像素' % (p['pixels'] / 1e3))

    # 點擊 reader → 拿焦點
    c.pointer_button(100, 200)
    print('點擊 (100,200) → 焦點 = %s' % c.surfaces[c.focus].title)
    c.key('j')
    print('鍵盤送給 → %s' % c.surfaces[c.focus].title)

    # 點面板不搶焦點
    c.pointer_button(500, 10)
    print('點面板 → 焦點仍是 %s' % (c.surfaces[c.focus].title if c.focus else None))

    # alt-tab
    c.focus_cycle()
    print('alt-tab → %s' % c.surfaces[c.focus].title)

    # 滾動 content
    a.scroll = 3
    c.damage(a.id, Rect(0, 22, 520, 358))
    p = c.plan()
    raster(c, p, '%s/comp2.png' % outdir)
    print('滾動 3 行 → 合成 %.0f K ＋ 底 %.0f K' % (p['pixels'] / 1e3, p['bg'] / 1e3))

    # 幀節流
    s = Scheduler(60)
    c.pending = []
    produced = 0
    for t in range(0, 201):
        if t % 2 == 0:
            c.damage_rect(Rect(100, 100, 120, 80))
        if s.poll(float(t), c):
            produced += 1
    print('200 ms 內 101 次 damage → %d 幀（60 Hz 理論上限 13）' % produced)

    # 全屏播放 → 直掃
    for x in (a, b, panel, cur):
        c.set_visible(x.id, False)
    b.visible = True
    b.x, b.y, b.w, b.h = 0, 0, c.w, c.h
    c.damage_rect(Rect(0, 0, c.w, c.h))
    p = c.plan()
    print('單一全屏不透明 → scanout=%s，合成 %d 像素'
          % (p['scanout'] is not None, p['pixels']))


if __name__ == '__main__':
    if '--demo' in sys.argv:
        i = sys.argv.index('--demo')
        demo(sys.argv[i + 1] if len(sys.argv) > i + 1 else 'out')
    else:
        print(__doc__)
