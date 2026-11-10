#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""reader v0.2 —— 論壇式的文字瀏覽器前端（建在 minireader 之上）

主線：文字頁像看論壇（列表 → 內文），媒體只給「連結＋簡介」，播放交給 mpv 小窗。

  python3 reader.py https://www.gnu.org/ --list
  python3 reader.py sample.html
  python3 reader.py sample.html --play 1        # 有 mpv 就開，沒有就印命令
  python3 reader.py https://site/page --open 3  # 從列表第 3 條跳過去
"""
import argparse
import re
import shutil
import subprocess
import sys
from urllib.parse import urlparse, urljoin

import minireader as mr

MPV_FMT = ("mpv --no-border --geometry=360x203 "
           "--input-ipc-server=/tmp/mini.mpv.sock -- '{url}'")

UA = ('Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 '
      '(KHTML, like Gecko) Chrome/120.0 Safari/537.36')


def mpv_cmd(url, info=None):
    """組播放命令。B 站要帶 Referer，否則 CDN 直接 403。"""
    import bili
    opts = ['--no-border', '--geometry=360x203',
            '--input-ipc-server=/tmp/mini.mpv.sock']
    if bili.is_bili(url):
        opts.append("--referrer='https://www.bilibili.com/'")
        opts.append("--user-agent='%s'" % UA)
    if info and info.get('audio'):
        opts.append("--audio-file='%s'" % info['audio'])
    return 'mpv %s -- \'%s\'' % (' '.join(opts), url)

SWAY_RULE = 'for_window [app_id="mpv"] floating enable, resize set 360 203, move position center'

HINTS = '操作：o <n> 開連結   v <n> 播媒體   l 重列   q 離開（GUI 版再做真的鍵位）'


def shorten(url, budget):
    """把 URL 縮到 budget 內：去 scheme、丟長 query，再從尾端省略。"""
    u = re.sub(r'^https?://', '', url)
    u = re.sub(r'\?.*$', '?…', u)
    u = re.sub(r'#.*$', '#…', u)
    if mr.dw(u) <= budget:
        return u
    return u[:max(4, budget - 1)] + '…'


def listing(f, width):
    """論壇式列表：編號 + 標題 + 連結。"""
    items = f.link_items or [(u, u) for u in f.links]
    if not items:
        return '（這一頁沒有可跟的連結）'
    nw = max(mr.dw(t) for t, _ in items)
    nw = min(nw, 30)
    out = []
    for i, (txt, url) in enumerate(items, 1):
        pad = ' ' * max(1, nw - mr.dw(txt) + 2)
        budget = max(12, width - nw - 9)
        out.append('%3d. %s%s%s' % (i, txt, pad, shorten(url, budget)))
    return '\n'.join(out)


def media_list(f, width):
    if not f.media:
        return None
    out = ['媒體 (%d)' % len(f.media)]
    for i, (tag, url) in enumerate(f.media, 1):
        out.append('  v%-2d [%s] %s' % (i, tag, shorten(url, max(12, width - 14))))
    return '\n'.join(out)


def play(url, dry=False):
    import resolver

    info = None
    if not dry and resolver.needs_resolve(url) and resolver.have_ytdlp():
        print('解析媒體頁面…')
        info = resolver.resolve(url)
        if info:
            print(resolver.card(info))
            url = info['stream'] or url

    cmd = mpv_cmd(url, info)
    if dry:
        print('[dry] 會執行：\n  ' + cmd)
        print('\nsway 浮窗規則：\n  ' + SWAY_RULE)
        return 0
    if not shutil.which('mpv'):
        print('尚未裝 mpv，指令如下：\n  ' + cmd)
        print('\nsway 浮窗規則：\n  ' + SWAY_RULE)
        if re.search(r'\.(m3u8|mpd)(\?|$)|/blob/|bilibili|youtube|youtu\.be', url):
            print('\n這個連結不是純檔案，要交給 yt-dlp 解析：')
            print("  mpv --ytdl-format='bv*[height<=720]+ba/b' --geometry=360x203 -- '%s'" % url)
        return 1
    subprocess.Popen(['sh', '-c', cmd], stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)
    print('已送出播放請求 → %s' % shorten(url, 60))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('src', help='HTML 檔或 http(s) URL')
    ap.add_argument('--width', type=int, default=76)
    ap.add_argument('--no-color', action='store_true')
    ap.add_argument('--no-chrome', action='store_true')
    ap.add_argument('--list', action='store_true', help='只列連結（論壇列表視圖）')
    ap.add_argument('--open', type=int, metavar='N', help='跟第 N 條連結')
    ap.add_argument('--play', type=int, metavar='N', help='播放第 N 個媒體')
    ap.add_argument('--dry', action='store_true', help='只印出播放命令，不執行')
    args = ap.parse_args(argv)

    color = not args.no_color
    html, base = mr.load(args.src)
    f = mr.parse(html, base, args.no_chrome)

    if args.play:
        if not f.media:
            print('這一頁沒有認出媒體喵')
            return 1
        if args.play > len(f.media):
            print('只有 %d 個媒體' % len(f.media))
            return 1
        return play(f.media[args.play - 1][1], args.dry)

    if args.list:
        title = ('─ %s ' % f.title.strip())[:args.width]
        print(title + '─' * max(0, args.width - mr.dw(title)))
        print(listing(f, args.width))
        return 0

    if args.open is not None:
        items = f.link_items or [(u, u) for u in f.links]
        if not 1 <= args.open <= len(items):
            print('列表只有 %d 條' % len(items))
            return 1
        target = urljoin(base or args.src, items[args.open - 1][1])
        print('# 跟隨 → %s\n' % target)
        html, base = mr.load(target)
        f = mr.parse(html, base, args.no_chrome)

    body, _ = mr.render(f, args.width, color)
    print(body)

    mlist = media_list(f, args.width)
    if mlist:
        print('\n' + '─' * args.width)
        print(mlist)
    print('\n' + '─' * args.width)
    print(HINTS)
    return 0


if __name__ == '__main__':
    sys.exit(main())
