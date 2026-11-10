#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tui.py —— 論壇式 TUI（curses）v0.3

核心是純函數 act(session, key) → action，畫面外殼很薄，所以能無終端自測。

  python3 tui.py sample.html            # 真的開 TUI
  python3 tui.py sample.html --selftest # 不開終端，驗鍵位邏輯

鍵位：j/k 或 ↑/↓ 移動　空格/J 翻頁　g/G 頭尾
      Enter/o 開連結　v 播媒體　l 列連結　b/Esc 返回　q 離開
"""
import argparse
import re
import sys

import minireader as mr
from reader import shorten

HINTS = ' j/k 移　⏎ 開　v 播　l 列連結　b 返　q 離 '


class View:
    def __init__(self, title, lines, links=None, media=None, media_line=None, flt=None):
        self.title = title
        self.lines = lines
        self.links = links or [None] * len(lines)
        self.media = media or []
        self.media_line = media_line or {}
        self.filter = flt
        self.cursor = 0
        self.top = 0


def doc_view(f, width):
    body, lines = mr.render(f, width, False)
    rows = body.split('\n')
    links = [next((r.href for _, r in segs if r and r.href), None) for _, segs, _ in lines]
    links += [None] * (len(rows) - len(links))
    mline = {}
    for i, ln in enumerate(rows):
        m = re.search(r'媒體 #(\d+)', ln)
        if m:
            mline[i] = int(m.group(1))
    return View(f.title.strip() or '（無標題）', rows, links, f.media, mline, f)


def list_view(f, width):
    items = f.link_items or [(u, u) for u in f.links]
    rows, links = [], []
    for i, (t, u) in enumerate(items, 1):
        rows.append('%3d. %s  %s' % (i, t[:28], shorten(u, max(10, width - 38))))
        links.append(u)
    if not rows:
        rows = ['（這一頁沒有可跟的連結）']
    return View('連結列表 · %s' % (f.title.strip() or '?'), rows, links, flt=f)


class Session:
    def __init__(self, width=76, drop_chrome=False):
        self.width = width
        self.drop_chrome = drop_chrome
        self.stack = []
        self.msg = ''

    def open(self, src, push=True):
        html, base = mr.load(src)
        f = mr.parse(html, base, self.drop_chrome)
        v = doc_view(f, self.width)
        if push:
            self.stack.append(v)
        else:
            self.stack = [v]
        self.msg = ''
        return v

    def open_url(self, url):
        self.open(url)


def act(s, key):
    """純函數：回傳 action（'redraw' / 'quit' / 'noop' / 'back' / 'list' / ('open',url) / ('play',n)）。"""
    v = s.stack[-1]
    n = len(v.lines)
    if key in ('q', 'Q'):
        return 'quit'
    if key in ('j', 'KEY_DOWN', 'n'):
        v.cursor = min(n - 1, v.cursor + 1)
        return 'redraw'
    if key in ('k', 'KEY_UP', 'p'):
        v.cursor = max(0, v.cursor - 1)
        return 'redraw'
    if key in (' ', 'J', 'KEY_NPAGE'):
        v.cursor = min(n - 1, v.cursor + 10)
        return 'redraw'
    if key == 'K':
        v.cursor = max(0, v.cursor - 10)
        return 'redraw'
    if key == 'g':
        v.cursor = 0
        return 'redraw'
    if key == 'G':
        v.cursor = max(0, n - 1)
        return 'redraw'
    if key in ('\n', '\r', 'KEY_ENTER', 'o'):
        url = v.links[v.cursor] if v.cursor < len(v.links) else None
        return ('open', url) if url else 'noop'
    if key == 'v':
        idx = v.media_line.get(v.cursor)
        if idx is None:
            for d in range(1, 7):
                if v.cursor - d in v.media_line:
                    idx = v.media_line[v.cursor - d]
                    break
        return ('play', idx) if idx else 'noop'
    if key == 'l':
        return 'list'
    if key in ('b', '\x1b', 'KEY_BACKSPACE', 'h'):
        return 'back'
    return 'noop'


def execute(s, action, dry_play=True):
    """回傳 False 表示要離開。"""
    if action == 'quit':
        return False
    if action == 'back':
        if len(s.stack) > 1:
            s.stack.pop()
        return True
    if action == 'list':
        f = s.stack[-1].filter
        if f:
            s.stack.append(list_view(f, s.width))
        return True
    if isinstance(action, tuple):
        kind, arg = action
        if kind == 'open':
            if not arg:
                s.msg = '這行沒有連結'
                return True
            try:
                s.open_url(arg)
            except Exception as e:
                s.msg = '開啟失敗：%s' % e
        elif kind == 'play':
            v = s.stack[-1]
            if not arg or arg > len(v.media):
                s.msg = '這行附近沒有媒體'
                return True
            tag, url = v.media[arg - 1]
            s.msg = '播放 #%d [%s] %s' % (arg, tag, shorten(url, 40))
            if not dry_play:
                import reader
                reader.play(url)
    return True


def selftest(path):
    s = Session(width=72)
    s.open(path, push=False)
    v = s.stack[0]
    print('載入：%d 行 / 連結 %d / 媒體 %d' % (len(v.lines), len(v.links), len(v.media)))
    print('媒體行：%s' % (v.media_line or '（無）'))
    print('連結行前三：%s' % [i for i, h in enumerate(v.links) if h][:3])
    steps = [('j', 1), ('j', 1), ('G', 1), ('g', 1), ('l', 1), ('b', 1)]
    for key, _ in steps:
        a = act(s, key)
        top = s.stack[-1]
        print('  %-6r → %-18r 視圖=%-14s cursor=%d/%d'
              % (key, a, top.title[:12], top.cursor, len(top.lines)))
        if not execute(s, a):
            break
    # 把游標放到已知的媒體行 / 連結行，驗 open 與 play
    v = s.stack[0]
    v.cursor = min(v.media_line)
    print('  %-6r → %r（媒體行 %d）' % ('v', act(s, 'v'), v.cursor))
    v.cursor = next(i for i, h in enumerate(v.links) if h)
    print('  %-6r → %r（連結行 %d）' % ('\\n', act(s, '\n'), v.cursor))
    execute(s, act(s, '\n'))
    print('  open 失敗訊息：%s' % s.msg)
    return 0


def draw(scr, s):
    import curses
    scr.erase()
    h, w = scr.getmaxyx()
    v = s.stack[-1]
    head = (' %s ' % v.title)[:max(1, w - 12)]
    scr.addstr(0, 0, head, curses.color_pair(1) | curses.A_BOLD)
    pos = '[%d/%d]' % (v.cursor + 1, len(v.lines))
    if w > len(pos) + 1:
        scr.addstr(0, w - len(pos), pos, curses.color_pair(1))
    body = max(1, h - 4)
    if v.cursor < v.top:
        v.top = v.cursor
    if v.cursor >= v.top + body:
        v.top = v.cursor - body + 1
    for i in range(body):
        idx = v.top + i
        if idx >= len(v.lines):
            break
        text = v.lines[idx][:w - 1]
        attr = curses.color_pair(2) if idx == v.cursor else 0
        scr.addstr(1 + i, 0, text.ljust(w - 1)[:w - 1], attr)
    scr.addstr(h - 3, 0, '─' * (w - 1))
    if s.msg:
        scr.addstr(h - 2, 0, s.msg[:w - 1], curses.color_pair(1))
    scr.addstr(h - 1, 0, HINTS[:w - 1])
    scr.refresh()


def run(s):
    import curses

    def _main(scr):
        curses.curs_set(0)
        try:
            curses.use_default_colors()
            curses.init_pair(1, curses.COLOR_CYAN, -1)
            curses.init_pair(2, curses.COLOR_BLACK, curses.COLOR_CYAN)
        except curses.error:
            pass
        while True:
            draw(scr, s)
            try:
                ch = scr.get_wch()
            except curses.error:
                continue
            key = ch if isinstance(ch, str) else curses.keyname(ch).decode()
            if key == 'KEY_RESIZE':
                continue
            if not execute(s, act(s, key), dry_play=True):
                return

    try:
        curses.wrapper(_main)
    except KeyboardInterrupt:
        pass


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('--width', type=int, default=76)
    ap.add_argument('--no-chrome', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest(args.src)

    s = Session(width=args.width, drop_chrome=args.no_chrome)
    s.open(args.src, push=False)
    run(s)
    return 0


if __name__ == '__main__':
    sys.exit(main())
