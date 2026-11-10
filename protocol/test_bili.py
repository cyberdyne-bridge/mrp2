#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_bili.py — B 站連結正規化的離線測試（8 種寫法 + 2 個反例）"""
import sys

import bili

CASES = [
    ('BV1xx411c7mD', 'https://www.bilibili.com/video/BV1xx411c7mD'),
    ('av170001', 'https://www.bilibili.com/video/av170001'),
    ('https://m.bilibili.com/video/BV1GJ411x7h7?p=2',
     'https://www.bilibili.com/video/BV1GJ411x7h7?p=2'),
    ('https://www.bilibili.com/video/av170001/?spm_id_from=333.999',
     'https://www.bilibili.com/video/av170001'),
    ('//player.bilibili.com/player.html?bvid=BV1GJ411x7h7&page=3',
     'https://www.bilibili.com/video/BV1GJ411x7h7?p=3'),
    ('https://player.bilibili.com/player.html?aid=170001&page=1',
     'https://www.bilibili.com/video/av170001'),
    ('https://b23.tv/abcd1234', 'https://b23.tv/abcd1234'),
    ('https://www.bilibili.com/video/BV1GJ411x7h7', 'https://www.bilibili.com/video/BV1GJ411x7h7'),
]

NEG = ['https://example.com/a.mp4', 'https://cdn.x/y.m3u8', '']


def main():
    ok = 0
    for src, want in CASES:
        got = bili.canonical(src)
        good = got == want
        ok += good
        print('%-58s %s' % (src[:56], 'PASS' if good else 'FAIL → %s' % got))
    for src in NEG:
        got = bili.canonical(src)
        good = got is None
        ok += good
        print('%-58s %s' % (src or '(空字串)', 'PASS' if good else 'FAIL → %s' % got))
    total = len(CASES) + len(NEG)
    print('\n%d/%d 通過' % (ok, total))
    return 0 if ok == total else 1


if __name__ == '__main__':
    sys.exit(main())
