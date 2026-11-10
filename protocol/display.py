#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""display.py — 顯示後端抽象（KMS 的兩張臉）

MockDisplay：記憶體裡的 framebuffer，可翻頁、可存 PNG。iSH 上跑這個。
DRMDisplay：真的 /dev/dri/card0。需要 libdrm（或手刻 ioctl），在真機才驗。

格式：真 KMS 用 XRGB8888（B,G,R,X），Mock 用 RGB888 省事；
轉換只是每個像素搬 3 bytes，等真機再補。
"""
import os
import struct

try:
    from render import save_png
except Exception:                                    # pragma: no cover
    save_png = None


class Display:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.stride = w * 3
        self.back = bytearray(self.stride * h)
        self.front = bytearray(self.stride * h)
        self.flips = 0

    def map(self):
        return self.back

    def flip(self):
        self.front[:] = self.back
        self.flips += 1

    def save(self, path):
        if save_png is None:
            raise RuntimeError('沒有 PNG 寫入器')
        save_png(path, self.w, self.h, bytes(self.front))

    def close(self):
        pass


class MockDisplay(Display):
    """純記憶體。翻頁＝整塊複製（真機會走 page flip 中斷）。"""
    pass


class DRMDisplay(Display):
    """真 KMS。刻意留成骨架：沒有 /dev/dri 的環境不該假裝能跑。"""

    DRM_IOCTL_MODE_GETRESOURCES = 0xC01064A0
    DRM_IOCTL_MODE_CREATE_DUMB = 0xC02064B2
    DRM_IOCTL_MODE_MAP_DUMB = 0xC01064B3
    DRM_IOCTL_MODE_ADDFB = 0xC01464AE
    DRM_IOCTL_MODE_PAGE_FLIP = 0xC01864B0

    def __init__(self, w, h, card='/dev/dri/card0'):
        super().__init__(w, h)
        if not os.path.exists(card):
            raise FileNotFoundError('沒有 %s：這台機器沒有 DRM/KMS' % card)
        raise NotImplementedError(
            'DRM 後端要等 Artix：libdrm 綁定 + dumb buffer + addfb + page flip。\n'
            'iSH 沒有 /dev/dri，所以這條路只能在真機驗。')
