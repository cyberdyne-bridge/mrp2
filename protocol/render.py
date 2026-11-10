#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""render.py — 前端渲染器（iSH 版：純標準庫軟體光柵）

Renderer 介面只有四個動作：rect / line / text / flush。
今天落 `SoftRenderer`（直接畫進 RGB 緩衝、輸出 PNG）；
`GLRenderer` 只留殼，等有 EGL 那天再填（quad batch + MSDF）。

  python3 minireader.py sample.html --emit --width 72 > /tmp/doc.json
  python3 render.py /tmp/doc.json -o preview.png --scale 2

注意：這版沒有真字型，CJK 用「方塊」表示——它是**版面檢視器**，
不是排版成品；真字要等 MSDF／FreeType 那一步。
"""
import json
import struct
import sys
import time
import zlib

from minireader import dw

BG = (0xFB, 0xFB, 0xF7)
INK = (0x2B, 0x2B, 0x2B)
DIM = (0x8A, 0x8A, 0x86)
RULE = (0xC8, 0xC8, 0xC2)
LINK = (0x1A, 0x5F, 0xD0)
CODE = (0xA8, 0x5A, 0x00)
QUOTE = (0x77, 0x7B, 0x82)
HEAD = (0x10, 0x10, 0x10)
CARD = (0x0A, 0x7A, 0x6A)


# ─────────────────────── 介面 ───────────────────────
class Renderer:
    """所有前端都吃同一份 display list，只換這個介面。"""

    def begin(self, w, h, bg=BG):
        raise NotImplementedError

    def rect(self, x, y, w, h, color):
        raise NotImplementedError

    def line(self, x, y, w, color, thick=1):
        raise NotImplementedError

    def text(self, x, y, s, color, scale=1, bold=False):
        raise NotImplementedError

    def flush(self, path):
        raise NotImplementedError


# ─────────────────── 軟體光柵（可跑） ───────────────────
class SoftRenderer(Renderer):
    def __init__(self, ink_only=False):
        self.w = self.h = 0
        self.buf = b''
        self.ink_only = ink_only
        self.px = 0          # 實際寫入的像素數（用來看「省了多少」）

    def begin(self, w, h, bg=BG):
        self.w, self.h = w, h
        self.buf = bytearray(bytes(bg) * (w * h))
        self.px = 0

    def rect(self, x, y, w, h, color):
        x, y = max(0, int(x)), max(0, int(y))
        w, h = int(w), int(h)
        if w <= 0 or h <= 0 or x >= self.w or y >= self.h:
            return
        w = min(w, self.w - x)
        h = min(h, self.h - y)
        row = bytes(color) * w
        stride = self.w * 3
        off = y * stride + x * 3
        self.px += w * h
        for _ in range(h):
            self.buf[off:off + w * 3] = row
            off += stride

    def line(self, x, y, w, color, thick=1):
        self.rect(x, y, w, thick, color)

    def text(self, x, y, s, color, scale=1, bold=False):
        cw, ch = 8 * scale, 12 * scale
        cx = x
        for c in s:
            n = dw(c)
            if self.ink_only:
                for dy, x0, x1 in glyph_runs(c, n * cw - scale, ch):
                    self.rect(cx + x0, y + dy, x1 - x0, 1, color)
            else:
                self.rect(cx, y, n * cw - scale, ch + (scale if bold else 0), color)
            cx += n * cw

    def blit565(self, x, y, w, h, data):
        """RGB565（little-endian）貼圖；慢但夠驗證用。"""
        for row in range(h):
            dy = y + row
            if dy < 0 or dy >= self.h:
                continue
            base = (dy * self.w + x) * 3
            src = row * w * 2
            for col in range(w):
                dx = x + col
                if dx < 0 or dx >= self.w:
                    continue
                p = src + col * 2
                v = data[p] | (data[p + 1] << 8)
                o = base + col * 3
                self.buf[o] = ((v >> 11) & 0x1F) * 255 // 31
                self.buf[o + 1] = ((v >> 5) & 0x3F) * 255 // 63
                self.buf[o + 2] = (v & 0x1F) * 255 // 31

    def flush(self, path):
        save_png(path, self.w, self.h, bytes(self.buf))


def glyph_runs(ch, w, h, coverage=0.22):
    """示意字形：確定性的橫向 run。真字型只要把這裡換成點陣圖集即可。"""
    seed = (ord(ch) * 2654435761) & 0xFFFFFFFF
    runs = []
    for dy in range(h):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        if (seed >> 16) % 100 < coverage * 100:
            x0 = (seed >> 5) % max(1, w - 2)
            runs.append((dy, x0, min(w, x0 + 2 + (seed % 3))))
    return runs


def save_png(path, w, h, rgb):
    rows = [b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h)]
    raw = zlib.compress(b''.join(rows), 6)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(_chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(_chunk(b'IDAT', raw))
        f.write(_chunk(b'IEND', b''))


def _chunk(tag, data):
    return (struct.pack('>I', len(data)) + tag + data +
            struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))


# ─────────────────── GL 殼（待填） ───────────────────
class GLRenderer(Renderer):
    """等 Artix 上有 EGL：字形圖集一次上傳，每幀只送 quad。"""

    def __init__(self):
        raise NotImplementedError('GL 後端要等有 EGL 的機器：quad batch + MSDF 字型')


# ─────────────────── 版面 → 幾何 ───────────────────
def _color(kind, seg):
    if seg.get('h'):
        return LINK
    if seg.get('c'):
        return CODE
    if kind in ('h1', 'h2', 'h3', 'h4', 'h5', 'h6'):
        return HEAD
    if kind == 'quote':
        return QUOTE
    if kind in ('rule', 'media', 'media-url', 'img'):
        return DIM
    return INK


BOX_H = set('─━')
BOX_V = set('│┃')
BOX_C = set('┌┐└┘├┤┬┴┼')


def _cell(r, x, y, ch, color, CW, LH, scale):
    """畫一格：框線字元畫線，其餘畫方塊（代表文字）。"""
    if ch in BOX_H:
        r.line(x, y + LH // 2, CW, RULE, thick=max(1, scale))
    elif ch in BOX_V:
        r.rect(x + CW // 2 - scale // 2, y, max(1, scale), LH, RULE)
    elif ch in BOX_C:
        r.rect(x, y + LH // 2, CW, max(1, scale), RULE)
    elif ch != ' ':
        r.text(x, y, ch, color, scale)


def paint(doc, scale=1, damage=None, r=None, ink_only=False):
    """回傳 (renderer, 統計)。damage=(y0,y1) 時只畫那個像素帶。"""
    CW, LH, M = 8 * scale, 16 * scale, 20 * scale
    W = M * 2 + doc['width'] * CW
    H = M * 2 + len(doc['lines']) * LH
    r = r or SoftRenderer(ink_only=ink_only)
    r.begin(W, H, BG)

    drawn = 0
    y = M
    for ln in doc['lines']:
        if damage and not (damage[0] <= y + LH and y <= damage[1]):
            y += LH
            continue
        drawn += 1
        x = M
        kind = ln['k']
        for seg in ln['s']:
            t = seg['t']
            color = DIM if seg.get('n') else _color(kind, seg)
            bold = kind.startswith('h')
            for ch in t:
                _cell(r, x, y, ch, color, CW, LH, scale)
                x += dw(ch) * CW
        y += LH
    return r, {'lines': drawn, 'w': W, 'h': H}


def main(argv=None):
    argv = argv or sys.argv[1:]
    if not argv:
        print(__doc__)
        return 2
    doc = json.load(open(argv[0], encoding='utf-8'))
    out = 'preview.png'
    scale = 1
    if '-o' in argv:
        out = argv[argv.index('-o') + 1]
    if '--scale' in argv:
        scale = int(argv[argv.index('--scale') + 1])

    ink = '--ink' in argv
    if '--dark' in argv:
        global BG, INK, DIM, RULE, LINK, CODE, QUOTE, HEAD
        BG, INK = (0x0B, 0x0B, 0x0C), (0xE8, 0xE8, 0xE4)
        DIM, RULE = (0x77, 0x77, 0x7A), (0x33, 0x33, 0x38)
        LINK, CODE = (0x6E, 0xA8, 0xFF), (0xE0, 0xA0, 0x40)
        QUOTE, HEAD = (0xA8, 0xAC, 0xB4), (0xFF, 0xFF, 0xFF)

    t = time.time()
    r, st = paint(doc, scale, ink_only=ink)
    t1 = time.time()
    r.flush(out)
    t2 = time.time()
    print('畫布 %dx%d ｜ %d 行 ｜ 墨水 %s ｜ 寫入 %.0f K 像素'
          % (st['w'], st['h'], st['lines'], '只畫墨' if ink else '滿格',
             r.px / 1000))
    print('光柵 %.1f ms ｜ 寫 PNG %.1f ms ｜ 輸出：%s'
          % ((t1 - t) * 1000, (t2 - t1) * 1000, out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
