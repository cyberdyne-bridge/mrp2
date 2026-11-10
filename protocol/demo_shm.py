#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""demo_shm.py — 甲的生產端：memfd + SCM_RIGHTS

建一塊 1920×1080 RGB565 的 memfd（4.1 MB），寫好漸層，
監聽 Unix socket，把 fd 交給 C 端（shmvid）。

  python3 demo_shm.py /tmp/mrp.sock &
  /var/tmp/shmvid /tmp/mrp.sock 60 300
"""
import mmap
import os
import socket
import struct
import sys

W, H = 1920, 1080
BPP = 4


def fill(mm):
    """每列一個顏色（夠驗證用，不追求畫面好看）。"""
    for y in range(H):
        r = 20 + 200 * y // H
        g = 30 + 180 * y // H
        b = 120
        if BPP == 4:
            px = bytes([b, g, r, 0xFF])
            mm[y * W * 4:(y + 1) * W * 4] = px * W
        else:
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            mm[y * W * 2:(y + 1) * W * 2] = bytes([v & 0xFF, v >> 8]) * W


def main(argv=None):
    argv = argv or sys.argv[1:]

    global BPP
    BPP = int(argv[2]) if len(argv) > 2 else 4
    size = W * H * BPP
    path = '/tmp/mrp-video.bin'
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o600)
    os.ftruncate(fd, size)
    print('（iSH 不支援 SCM_RIGHTS：用同一檔案 mmap 驗證零拷貝）')
    mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    fill(mm)
    print('共享記憶體：%.1f MB（%dx%d，%d bpp）' % (size / 1048576, W, H, BPP * 8))

    import subprocess
    hz = argv[0] if argv else '0'
    frames = argv[1] if len(argv) > 1 else '200'
    print('交給 C：%s（%d Hz，%s 幀）' % (path, int(hz), frames))
    extra = [argv[3]] if len(argv) > 3 else []
    return subprocess.call(['/var/tmp/shmvid', path, hz, frames, str(W), str(H)] + extra)


if __name__ == '__main__':
    sys.exit(main())
