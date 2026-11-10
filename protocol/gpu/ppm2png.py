#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ppm2png.py — 把 P6 PPM 轉 PNG（純標準庫，給 GPU 輸出用）"""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    if not data.startswith(b'P6'):
        raise ValueError('不是 P6 PPM')
    idx, fields = 2, []
    while len(fields) < 3:
        while idx < len(data) and data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b'#':
            while data[idx:idx + 1] != b'\n':
                idx += 1
            continue
        start = idx
        while idx < len(data) and not data[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(data[start:idx]))
    idx += 1
    w, h, _ = fields
    return w, h, data[idx:idx + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        return (struct.pack('>I', len(body)) + tag + body +
                struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF))

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))


def main(argv=None):
    argv = argv or sys.argv[1:]
    if len(argv) < 2:
        print('用法：ppm2png.py in.ppm out.png')
        return 2
    w, h, rgb = read_ppm(argv[0])
    write_png(argv[1], w, h, rgb)
    print('%s → %s（%dx%d）' % (argv[0], argv[1], w, h))
    return 0


if __name__ == '__main__':
    sys.exit(main())
