#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mkfont.py — 把 Unifont 烘成 MRPF 字模 ROM（1bpp）

格式（little-endian）：
  magic 'MRPF' | version u16 | count u32 | data_off u32
  index: count × (cp u32, off u32, w u8, h u8, pitch u8, pad u8)   ← 按 cp 排序
  data : 每個字形 pitch×h bytes 的 1bpp 點陣

  python3 mkfont.py out/font.rom [額外字集]
"""
import json
import struct
import sys

import freetype

FONT = '/usr/share/fonts/opentype/unifont/unifont.otf'


def build(chars):
    face = freetype.Face(FONT)
    face.set_pixel_sizes(0, 16)
    asc = face.size.ascender >> 6          # 基線到頂
    glyphs = {}
    for ch in sorted(set(chars)):
        cp = ord(ch)
        try:
            face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
        except Exception:
            continue
        b = face.glyph.bitmap
        left, top = face.glyph.bitmap_left, face.glyph.bitmap_top
        adv = face.glyph.advance.x >> 6
        cw = 16 if adv > 8 else 8          # 全形／半形
        rows = [0] * 16
        for y in range(b.rows):
            cy = asc - top + y
            if cy < 0 or cy >= 16:
                continue
            for x in range(b.width):
                if b.buffer[y * b.pitch + (x >> 3)] & (0x80 >> (x & 7)):
                    cx = left + x
                    if 0 <= cx < cw:
                        rows[cy] |= 1 << (cw - 1 - cx)
        pitch = (cw + 7) // 8
        data = bytearray()
        for r in rows:
            for i in range(pitch):
                data.append((r >> (8 * (pitch - 1 - i))) & 0xFF)
        glyphs[cp] = (cw, 16, pitch, bytes(data))
    return glyphs


def write_rom(path, glyphs):
    cps = sorted(glyphs)
    index_size = len(cps) * 12
    data_off = 12 + index_size
    out = bytearray()
    out += b'MRPF' + struct.pack('<HI', 1, len(cps)) + struct.pack('<I', data_off)
    data = bytearray()
    for cp in cps:
        w, h, pitch, buf = glyphs[cp]
        out += struct.pack('<IIBBBB', cp, len(data), w, h, pitch, 0)
        data += buf
    out += data
    open(path, 'wb').write(out)
    return len(out)


def main(argv=None):
    argv = argv or sys.argv[1:]
    out = argv[0] if argv else 'out/font.rom'
    chars = set(chr(c) for c in range(0x20, 0x7F))          # ASCII
    chars |= set('，。、；：！？「」『』（）《》…—·')
    try:
        doc = json.load(open('out/doc.json', encoding='utf-8'))
        for ln in doc['lines']:
            for seg in ln['s']:
                chars |= set(seg['t'])
    except Exception:
        pass
    # 常用字補一批（GB2312 一級前 500）
    for i in range(500):
        try:
            chars.add(bytes([0xB0 + i // 94, 0xA1 + i % 94]).decode('gb2312'))
        except Exception:
            pass
    chars = {c for c in chars if c.strip()}
    g = build(chars)
    n = write_rom(out, g)
    print('字形 %d 個 → %s（%.1f KB）' % (len(g), out, n / 1024))
    return 0


if __name__ == '__main__':
    sys.exit(main())
