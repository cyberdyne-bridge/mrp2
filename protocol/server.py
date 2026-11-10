#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""server.py — MRP2 伺服器：協議 → 合成 → 顯示，外加輸入分發與幀回呼"""
import proto
import render
from compositor import Compositor, Rect, Surface
from display import MockDisplay

ROLE_LAYER = {0: 'background', 1: 'normal', 2: 'top', 3: 'overlay', 4: 'normal'}


class Server:
    def __init__(self, w, h, display=None, refresh=60):
        self.w, self.h = w, h
        self.refresh = refresh
        self.comp = Compositor(w, h)
        self.display = display or MockDisplay(w, h)
        self.ops_seen = 0
        self.spans_seen = 0
        self.serial = 0
        self.client_caps = 0
        self.out = []                  # 待送回 client 的事件
        self.errors = []
        # 持續存在的畫布：每幀只補髒區，不是整張重畫（真機就是雙緩衝）
        self.canvas = render.SoftRenderer()
        self.canvas.begin(w, h, self.comp.bg)

    # ── 協議 ──
    def feed(self, data):
        ops = proto.decode(data)
        self.ops_seen += len(ops)
        for op in ops:
            k = op[0]
            if k == 'hello':
                self.client_caps = op[2]
                self.out.append(('hello_ack', proto.VERSION, proto.CAP_SPANS,
                                 self.serial, self.w, self.h, self.refresh))
            elif k == 'surf':
                self._surf(op)
            elif k == 'spans':
                self._spans(op)
            elif k == 'damage':
                _, sid, x, y, w, h = op
                if sid in self.comp.surfaces:
                    self.comp.damage(sid, Rect(x, y, w, h))
            elif k == 'buffer':
                self._buffer(op)
            elif k == 'commit':
                sid = op[1]
                if sid in self.comp.surfaces and (self.client_caps & proto.CAP_FRAME_CALLBACK):
                    self.out.append(('frame', sid, self.serial))
                    self.serial = (self.serial + 1) & 0xFFFFFFFF
            elif k == 'frame_ack':
                pass                                   # 客戶端回報，v2 只記錄
            elif k == 'unmap':
                if op[1] in self.comp.surfaces:
                    self.comp.set_visible(op[1], False)
            elif k in ('key', 'pointer', 'focus', 'frame', 'configure', 'error'):
                self.errors.append('client 不該送事件：%s' % k)
        return len(ops)

    def _surf(self, op):
        _, sid, x, y, w, h, z, layer, flags, role, parent = op
        layer = ROLE_LAYER.get(role, 'normal') if role else layer
        s = self.comp.surfaces.get(sid)
        if s is None:
            s = Surface(sid, 's%d' % sid, x, y, w, h, (246, 246, 242),
                        z=z, layer=layer, opaque=bool(flags & 1),
                        input=bool(flags & 4), keyboard=bool(flags & 4))
            s.spans = []
            s.role = role
            s.parent = parent
            self.comp.surfaces[sid] = s
            self.comp.seq = max(self.comp.seq, sid)
            self.comp.damage_rect(Rect(x, y, w, h))
        else:
            self.comp.damage(sid)
            s.x, s.y, s.w, s.h, s.z = x, y, w, h
            s.layer = layer
            s.opaque = bool(flags & 1)
            s.visible = not (flags & 2)
            s.role, s.parent = role, parent
            self.comp.damage(sid)

    def _spans(self, op):
        _, sid, tx, ty, spans = op
        s = self.comp.surfaces.get(sid)
        if s is None:
            return
        abs_spans = [(ty * 16 + y, x0, x1, c) for (y, x0, x1, c) in spans]
        s.spans = getattr(s, 'spans', []) + abs_spans
        self.spans_seen += len(spans)
        self.comp.damage(sid, Rect(0, 22 + ty * 16, s.w, 16))

    def _buffer(self, op):
        _, sid, ty, fmt, w, h, data = op
        s = self.comp.surfaces.get(sid)
        if s is None or fmt != 0:
            return
        if not hasattr(s, 'bands'):
            s.bands = {}
        s.bands[ty] = (w, h, data)
        self.comp.damage(sid, Rect(0, 22 + ty * 16, w, h))

    # ── 輸入 ──
    def inject_pointer(self, x, y, button=1, state=1):
        if state == 1:
            sid = self.comp.pointer_button(x, y, button)
        else:                                   # 放開不改焦點
            hit = self.comp.hit(x, y)
            sid = hit.id if hit else None
            self.comp.events.append(('click', sid, x, y))
        self.out.append(('pointer', sid or 0, x, y, button, state, self.serial))
        self.serial = (self.serial + 1) & 0xFFFFFFFF
        return sid

    def inject_pointer_move(self, x, y):
        hit = self.comp.hit(x, y)
        sid = hit.id if hit else 0
        self.out.append(('pointer', sid, x, y, 0, 0, self.serial))
        self.serial = (self.serial + 1) & 0xFFFFFFFF
        return sid

    def inject_key(self, keysym, state=1, mods=0):
        sid = self.comp.focus or 0
        self.comp.events.append(('key', sid or None, keysym))
        self.out.append(('key', sid, self.serial, keysym, state, mods))
        self.serial = (self.serial + 1) & 0xFFFFFFFF
        return sid

    def set_focus(self, sid):
        self.comp.set_focus(sid)
        self.out.append(('focus', sid or 0, 1))

    # ── 輸出 ──
    def flush_events(self):
        data = proto.encode_events(self.out)
        self.out = []
        return data

    def frame(self, path=None):
        plan = self.comp.plan()
        r = self.canvas
        for d in plan['damage']:
            r.rect(d.x, d.y, d.w, d.h, self.comp.bg)
        if plan['scanout']:
            s = self.comp.surfaces[plan['scanout']]
            r.rect(s.x, s.y, s.w, s.h, s.color)
        else:
            for op in plan['ops']:
                self._paint(r, self.comp.surfaces[op['surface']], op['rect'])
        self.display.front[:] = r.buf
        self.display.flips += 1
        if path:
            self.display.save(path)
        return plan

    def _paint(self, r, s, clip):
        r.rect(clip.x, clip.y, clip.w, clip.h, s.color)
        if getattr(s, 'role', 1) != 0:
            bar = Rect(s.x, s.y, s.w, 22).intersect(clip)
            if bar:
                r.rect(bar.x, bar.y, bar.w, bar.h, (224, 228, 236))
                r.text(bar.x + 6, bar.y + 5, s.title[:max(1, (bar.w - 10) // 8)],
                       (32, 32, 36), 1)
        for ty, (bw, bh, data) in getattr(s, 'bands', {}).items():
            r.blit565(s.x, s.y + 22 + ty * 16, bw, bh, data)
        for (y, x0, x1, ci) in getattr(s, 'spans', []):
            seg = Rect(s.x + x0, s.y + 22 + y, x1 - x0, 7).intersect(clip)
            if seg:
                r.rect(seg.x, seg.y, seg.w, seg.h, proto.PALETTE[ci % len(proto.PALETTE)])
