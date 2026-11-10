#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bili.py — B 站連結的正規化（離線可測，不打網路）

為什麼需要：B 站的播放器是 JS，頁面裡沒有 <video>；第三方站又是用
<iframe src="//player.bilibili.com/player.html?bvid=BV..."> 嵌進來的。
所以第一步不是解碼，是「把五六種寫法收斂成一種標準 URL」。

  BV1xx411c7mD
  av170001
  https://m.bilibili.com/video/BV1xx411c7mD?p=2
  https://www.bilibili.com/video/av170001/?spm_id_from=xxx
  //player.bilibili.com/player.html?bvid=BV1xx411c7mD&page=3
  https://b23.tv/abcd1234          （短鏈，要聯網才知去向）
"""
import re
from urllib.parse import parse_qs, urlparse

BV_RE = re.compile(r'(BV[0-9A-Za-z]{10})')
AV_RE = re.compile(r'av(\d+)', re.I)
HOSTS = ('bilibili.com', 'b23.tv', 'bili2233.cn')


def is_bili(url):
    if not url:
        return False
    u = url.strip()
    if BV_RE.fullmatch(u) or AV_RE.fullmatch(u):
        return True
    host = urlparse(u).netloc.lower() if '://' in u else u.lower()
    return any(h in host for h in HOSTS)


def is_short(url):
    return 'b23.tv' in url or 'bili2233.cn' in url


def canonical(url):
    """回標準 video URL；認不出來就回 None。短鏈原樣回傳（等 yt-dlp 跟隨）。"""
    if not url:
        return None
    u = url.strip()

    if is_short(u):
        return u if u.startswith('http') else 'https://' + u

    mq = re.search(r'\?([^#]*)', u)
    q = parse_qs(mq.group(1)) if mq else {}
    page = (q.get('page') or q.get('p') or ['1'])[0]

    def build(vid):
        return 'https://www.bilibili.com/video/%s%s' % (vid, '' if page == '1' else '?p=%s' % page)

    if 'player.bilibili.com' in u:
        mb = BV_RE.search(u)
        if mb:
            return build(mb.group(1))
        aid = (q.get('aid') or [''])[0]
        if aid.isdigit():
            return build('av' + aid)
        ma = AV_RE.search(u)
        return build('av' + ma.group(1)) if ma else None

    mb = BV_RE.search(u)
    if mb:
        return build(mb.group(1))

    ma = AV_RE.search(u)
    if ma and (is_bili(u) or u.lower().startswith('av')):
        return build('av' + ma.group(1))

    return None


if __name__ == '__main__':
    import sys
    for arg in sys.argv[1:]:
        print('%-60s → %s' % (arg[:58], canonical(arg)))
