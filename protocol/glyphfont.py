#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""glyphfont.py — 點陣字型：從 Unifont 烘成 1bpp 位圖，並用「墨段」畫文章

  python3 glyphfont.py out/doc.json out/glyph.png
"""
import json
import sys
import time

import freetype

from render import save_png
from minireader import dw

FONT = '/usr/share/fonts/opentype/unifont/unifont.otf'
BG = (0xFA, 0xFA, 0xF6)
INK = (0x1E, 0x1E, 0x22)
LINK = (0x14, 0x5A, 0xC8)


class BitmapFont:
    def __init__(self, path=FONT, px=16):
        self.face = freetype.Face(path)
        self.face.set_pixel_sizes(0, px)
        self.px = px
        self.cache = {}
        self.bytes = 0

    def glyph(self, ch):
        if ch in self.cache:
            return self.cache[ch]
        self.face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
        b = self.face.glyph.bitmap
        w, h, pitch = b.width, b.rows, b.pitch
        buf = bytes(b.buffer)
        rows = []
        for y in range(h):
            v = 0
            for x in range(w):
                byte = buf[y * pitch + (x >> 3)]
                if byte & (0x80 >> (x & 7)):
                    v |= 1 << (w - 1 - x)
            rows.append(v)
        g = (w, h, rows)
        self.cache[ch] = g
        self.bytes += (w * h + 7) // 8          # 1bpp 實際位元組
        return g


class Canvas:
    def __init__(self, w, h, bg=BG):
        self.w, self.h = w, h
        self.buf = bytearray(bytes(bg) * (w * h))
        self.ink = 0

    def rect(self, x, y, w, h, color):
        if w <= 0 or h <= 0:
            return
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(self.w, x + w), min(self.h, y + h)
        if x1 <= x0 or y1 <= y0:
            return
        row = bytes(color) * (x1 - x0)
        for yy in range(y0, y1):
            off = (yy * self.w + x0) * 3
            self.buf[off:off + (x1 - x0) * 3] = row
        self.ink += (x1 - x0) * (y1 - y0)

    def glyph(self, font, ch, x, y, color, scale=1):
        g = font.glyph(ch)
        if not g:
            return
        w, h, rows = g
        for ry, bits in enumerate(rows):
            rx = 0
            while rx < w:
                if bits & (1 << (w - 1 - rx)):
                    start = rx
                    while rx < w and (bits & (1 << (w - 1 - rx))):
                        rx += 1
                    self.rect(x + start * scale, y + ry * scale,
                              (rx - start) * scale, scale, color)
                else:
                    rx += 1


def render_doc(doc, out, scale=2, margin=(60, 60), lh=16, cw=8):
    font = BitmapFont()
    W, H = 1920, 1080
    c = Canvas(W, H)
    y = margin[1]
    glyphs = 0
    t0 = time.time()
    for ln in doc['lines']:
        x = margin[0]
        kind = ln['k']
        color = LINK if any(s.get('h') for s in ln['s']) else INK
        for seg in ln['s']:
            for ch in seg['t']:
                if ch.strip():
                    c.glyph(font, ch, x, y, color, scale)
                    glyphs += 1
                x += dw(ch) * cw * scale
        y += lh * scale
        if y > H - margin[1]:
            break
    dt = (time.time() - t0) * 1000
    save_png(out, W, H, bytes(c.buf))
    return {'glyphs': glyphs, 'ink': c.ink, 'ms': dt,
            'font_bytes': font.bytes, 'cache': len(font.cache), 'w': W, 'h': H}


def main(argv=None):
    argv = argv or sys.argv[1:]
    doc = json.load(open(argv[0], encoding='utf-8')) if argv else None
    out = argv[1] if len(argv) > 1 else 'out/glyph.png'
    st = render_doc(doc, out)
    total = st['w'] * st['h']
    print('畫布 %dx%d ＝ %.2f M 像素' % (st['w'], st['h'], total / 1e6))
    print('字形 %d 個（實際載入 %d）｜ 字型快取 %.1f KB'
          % (st['glyphs'], st['cache'], st['font_bytes'] / 1024))
    print('墨水覆蓋 %.0f K 像素（%.2f%%）' % (st['ink'] / 1e3, 100 * st['ink'] / total))
    print('渲染耗時 %.0f ms ｜ 每字 %.3f ms ｜ 輸出 %s' % (st['ms'], st['ms'] / st['glyphs'], out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
