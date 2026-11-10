#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""minireader v0.1 —— 最小復現：抓頁 → 過濾器 → 塊級排版 → 終端渲染

純標準庫、零依賴。
  python3 minireader.py sample.html --width 72 --stats
  python3 minireader.py https://example.com --no-chrome
"""
import argparse
import os
import re
import sys
import time
import unicodedata
from html.parser import HTMLParser
from urllib.parse import urljoin
from urllib.request import Request, urlopen

try:
    import bili
except Exception:  # bili.py 不在就叫外掛缺席，不影響主流程
    bili = None

# ────────────────────────── 0. 常量表 ──────────────────────────
SKIP_TAGS = {'script', 'style', 'noscript', 'svg', 'canvas', 'template', 'object', 'embed'}
CHROME_TAGS = {'nav', 'header', 'footer', 'aside'}
BLOCKISH = {'p', 'div', 'section', 'article', 'main', 'figure', 'figcaption',
            'dl', 'dt', 'dd', 'center', 'address', 'tr'}
HEADINGS = {'h1', 'h2', 'h3', 'h4', 'h5', 'h6'}
NO_START = set('，。、；：！？）》」』】〉·…—')


# ────────────────────── 1. 過濾器：HTML → 塊 ──────────────────────
class Run:
    """一段同款式的行內文字。"""
    __slots__ = ('text', 'b', 'i', 'code', 'href')

    def __init__(self, text, b=False, i=False, code=False, href=None):
        self.text = text
        self.b, self.i, self.code, self.href = b, i, code, href

    def same(self, o):
        return (self.b, self.i, self.code, self.href) == (o.b, o.i, o.code, o.href)


class Block:
    """一個塊級單元。"""
    __slots__ = ('kind', 'runs', 'level', 'extra')

    def __init__(self, kind, runs=None, level=0, extra=None):
        self.kind = kind
        self.runs = runs or []
        self.level = level
        self.extra = extra or {}


class BlockFilter(HTMLParser):
    def __init__(self, base='', drop_chrome=False):
        super().__init__(convert_charrefs=True)
        self.base = base
        self.drop_chrome = drop_chrome
        self.blocks = []
        self.runs = []
        self.kind = 'p'
        self.level = 0
        self.bold = self.ital = self.code = 0
        self.href = None
        self.skip = 0
        self.chrome = 0
        self.pre = 0
        self.liststack = []
        self.links = []
        self.link_items = []   # [(文字, href)]，給論壇式列表用
        self.media = []
        self.title = ''
        self._title = False

    # ---- 內部 ----
    def push(self, text):
        if not text:
            return
        new = Run(text, bool(self.bold), bool(self.ital), bool(self.code), self.href)
        if self.runs and self.runs[-1].same(new):
            self.runs[-1].text += text
        else:
            self.runs.append(new)

    def flush(self):
        if not self.runs:
            return
        txt = ''.join(r.text for r in self.runs)
        if txt.strip() or self.kind == 'pre':
            self.blocks.append(Block(self.kind, self.runs, self.level))
        self.runs = []

    # ---- 事件 ----
    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag in SKIP_TAGS:
            self.skip += 1
            return
        if self.skip:
            return
        if tag == 'title':
            self._title = True
            return
        if tag in ('meta', 'link', 'base'):
            return
        if tag in CHROME_TAGS and self.drop_chrome:
            self.flush()
            self.chrome += 1
            return
        if self.chrome:
            return

        if tag in ('video', 'audio', 'source', 'iframe'):
            src = (a.get('src') or a.get('data-src') or '').strip()
            if src:
                self.flush()
                url = urljoin(self.base, src)
                if bili is not None:
                    cu = bili.canonical(url)
                    if cu:
                        url, tag = cu, 'bilibili'
                self.media.append((tag, url))
                self.blocks.append(Block('media', [Run('〔%s〕' % tag)], 0, {'url': url}))
            return
        if tag == 'img':
            self.flush()
            alt = (a.get('alt') or '').strip()
            src = (a.get('src') or '').strip()
            self.blocks.append(Block('img', [Run(alt)], 0, {'src': urljoin(self.base, src)}))
            return
        if tag == 'br':
            self.push('\n')
            return
        if tag == 'hr':
            self.flush()
            self.blocks.append(Block('hr'))
            self.kind = 'p'
            return

        if tag in ('b', 'strong', 'th'):
            self.bold += 1
        elif tag in ('i', 'em'):
            self.ital += 1
        elif tag in ('code', 'kbd', 'samp'):
            self.code += 1
        elif tag == 'a':
            h = (a.get('href') or '').strip()
            self.href = urljoin(self.base, h) if h else None
            self._a_text = ''
            if self.href and not self.href.startswith('javascript:') and self.href not in self.links:
                self.links.append(self.href)

        if tag in HEADINGS:
            self.flush()
            self.kind = tag
        elif tag == 'tr':
            self.flush()
            self.kind = 'tr'
        elif tag in ('ul', 'ol'):
            self.flush()
            self.liststack.append(tag)
            self.kind = 'p'
        elif tag == 'li':
            self.flush()
            self.kind = 'li'
            self.level = max(0, len(self.liststack) - 1)
        elif tag == 'blockquote':
            self.flush()
            self.kind = 'quote'
        elif tag == 'pre':
            self.flush()
            self.pre += 1
            self.kind = 'pre'
        elif tag in ('td', 'th'):
            if self.runs:
                self.push('  |  ')
        elif tag in BLOCKISH:
            self.flush()
            self.kind = 'p'

    def handle_endtag(self, tag):
        if tag in SKIP_TAGS:
            self.skip = max(0, self.skip - 1)
            return
        if self.skip:
            return
        if tag == 'title':
            self._title = False
            return
        if tag in CHROME_TAGS and self.drop_chrome:
            if self.chrome:
                self.chrome -= 1
                self.flush()
                self.kind = 'p'
            return
        if self.chrome:
            return

        if tag in ('b', 'strong', 'th'):
            self.bold = max(0, self.bold - 1)
        elif tag in ('i', 'em'):
            self.ital = max(0, self.ital - 1)
        elif tag in ('code', 'kbd', 'samp'):
            self.code = max(0, self.code - 1)
        elif tag == 'a':
            txt = re.sub(r'\s+', ' ', self._a_text).strip()
            if self.href and txt:
                self.link_items.append((txt, self.href))
            self._a_text = ''
            self.href = None

        if tag in HEADINGS or tag in BLOCKISH or tag in ('blockquote', 'li'):
            self.flush()
            self.kind = 'p'
            self.level = 0
        elif tag == 'pre':
            self.flush()
            self.pre = max(0, self.pre - 1)
            self.kind = 'p'
        elif tag in ('ul', 'ol'):
            self.flush()
            if self.liststack:
                self.liststack.pop()
            self.kind = 'p'

    def handle_data(self, data):
        if self._title:
            self.title += data
            return
        if self.skip or self.chrome:
            return
        self.push(data if self.pre else re.sub(r'\s+', ' ', data))
        if self.href:
            self._a_text += data


# ─────────────────────── 2. 排版：塊 → 行 ───────────────────────
def is_wide(ch):
    return unicodedata.east_asian_width(ch) in ('W', 'F')


def dw(s):
    n = 0
    for c in s:
        if is_wide(c):
            n += 2
        elif unicodedata.combining(c):
            n += 0
        else:
            n += 1
    return n


def atoms(runs):
    """切成可換行的最小單位：CJK 單字、拉丁詞、空白、硬換行。
    禁則標點黏進前一個原子，避免它跑到行首。"""
    raw = []
    for r in runs:
        buf = []
        for ch in r.text:
            if ch == '\n':
                if buf:
                    raw.append((''.join(buf), r, 'word'))
                    buf = []
                raw.append(('\n', r, 'nl'))
            elif ch.isspace():
                if buf:
                    raw.append((''.join(buf), r, 'word'))
                    buf = []
                raw.append((' ', r, 'space'))
            elif is_wide(ch):
                if buf:
                    raw.append((''.join(buf), r, 'word'))
                    buf = []
                raw.append((ch, r, 'wide'))
            else:
                buf.append(ch)
        if buf:
            raw.append((''.join(buf), r, 'word'))

    out = []
    for text, run, kind in raw:
        if kind == 'wide' and text in NO_START and out and out[-1][2] in ('wide', 'word'):
            out[-1] = (out[-1][0] + text, out[-1][1], out[-1][2])
        else:
            out.append((text, run, kind))
    return out


def wrap(runs, width, first='', cont='', hard=False):
    """回傳 [(前綴, [(文字, run|None)])]。"""
    out = []
    if hard:  # pre：不重排，只加前綴
        cur = []
        for r in runs:
            for i, part in enumerate(r.text.split('\n')):
                if i:
                    out.append((cont, cur))
                    cur = []
                if part:
                    cur.append((part, r))
        out.append((cont, cur))
        return out

    line, w, pref = [], 0, first
    lim = width - dw(pref)

    def flush():
        nonlocal line, w, pref, lim
        while line and line[-1][0] == ' ':
            line.pop()
            w -= 1
        out.append((pref, line))
        line, w, pref = [], 0, cont
        lim = width - dw(pref)

    for text, run, kind in atoms(runs):
        if kind == 'nl':
            flush()
            continue
        if kind == 'space':
            if line:
                line.append((' ', run))
                w += 1
            continue
        tw = dw(text)
        if w + tw > lim and line:
            flush()
        if tw > lim and not line:  # 超長原子（網址）硬切
            chunk, cw = '', 0
            for ch in text:
                c2 = dw(ch)
                if cw + c2 > lim and chunk:
                    line.append((chunk, run))
                    w = cw
                    flush()
                    chunk, cw = '', 0
                chunk += ch
                cw += c2
            if chunk:
                line.append((chunk, run))
                w = cw
            continue
        line.append((text, run))
        w += tw
    if line:
        flush()
    return out


def split_cells(runs):
    """把一行表格的 runs 按 '  |  ' 切成若干格。"""
    SEP = '  |  '
    cells, cur = [], []
    for r in runs:
        for i, part in enumerate(r.text.split(SEP)):
            if i:
                cells.append(cur)
                cur = []
            if part:
                cur.append(Run(part, r.b, r.i, r.code, r.href))
    cells.append(cur)
    return cells


def table_rows(rows):
    """回傳 (文字行段列表, 欄寬)。最後一欄不補齊。"""
    grid = [split_cells(b.runs) for b in rows]
    ncol = max(len(c) for c in grid)
    widths = [0] * ncol
    for c in grid:
        for i, cell in enumerate(c):
            widths[i] = max(widths[i], dw(''.join(r.text for r in cell)))
    out = []
    for c in grid:
        segs = []
        for i in range(len(c)):
            cell = c[i]
            cw = dw(''.join(r.text for r in cell))
            for r in cell:
                segs.append((r.text, r))
            if i < len(c) - 1:
                segs.append((' ' * (widths[i] - cw) + ' │ ', None))
        # 剪掉行尾空白（缺欄的行不該拖一條尾巴）
        while segs:
            if not segs[-1][0].strip():
                segs.pop()
                continue
            t = segs[-1][0]
            if t != t.rstrip():
                segs[-1] = (t.rstrip(), segs[-1][1])
            break
        out.append((segs, widths))
    has_head = any(r.b for r in grid[0][0]) if grid and grid[0] else False
    return out, ncol, has_head


def layout(blocks, width):
    lines = []  # [前綴, 段列表, 種類]

    def add(pref, segs, kind):
        lines.append([pref, segs, kind])

    i = 0
    midx = 0
    while i < len(blocks):
        b = blocks[i]
        k = b.kind

        if k == 'tr':  # 連續 tr 合成一張表
            rows = []
            while i < len(blocks) and blocks[i].kind == 'tr':
                rows.append(blocks[i])
                i += 1
            grid, ncol, has_head = table_rows(rows)
            for n, (segs, widths) in enumerate(grid):
                add('', segs, 'tr')
                if n == 0 and has_head:
                    rule = '─┼─'.join('─' * w for w in widths)
                    add('', [(rule, None)], 'rule')
            add('', [], 'gap')
            continue

        if k == 'hr':
            add('', [('─' * width, None)], 'rule')
        elif k == 'img':
            alt = ''.join(r.text for r in b.runs)
            add('', [('〔圖〕%s' % (alt or '(無 alt)'), None)], 'img')
        elif k == 'media':
            midx += 1
            tag = b.runs[0].text.strip('〔〕')
            url = b.extra.get('url', '')
            head = '媒體 #%d · %s' % (midx, tag)
            add('', [('┌ %s %s' % (head, '─' * max(1, width - dw(head) - 3)), None)], 'media')
            for p, segs in wrap([Run(url)], width, first='│ ', cont='│ ', hard=True):
                add(p, segs, 'media-url')
            add('', [('│ ↳ 小窗播放：v%d' % midx, None)], 'media')
            add('', [('└' + '─' * max(0, width - 1), None)], 'media')
            add('', [], 'gap')
        elif k == 'pre':
            for p, segs in wrap(b.runs, width - 2, cont='  ', hard=True):
                add(p, segs, 'pre')
            add('', [], 'gap')
        elif k in HEADINGS:
            add('', [], 'gap')
            for p, segs in wrap(b.runs, width, cont='  '):
                add(p, segs, k)
        elif k == 'li':
            ind = '  ' * b.level
            for p, segs in wrap(b.runs, width, first=ind + '• ', cont=ind + '  '):
                add(p, segs, 'li')
        elif k == 'quote':
            for p, segs in wrap(b.runs, width, first='│ ', cont='│ '):
                add(p, segs, 'quote')
        else:
            for p, segs in wrap(b.runs, width):
                add(p, segs, 'p')
            add('', [], 'gap')
        i += 1

    # 收合連續空行、去頭尾
    merged = []
    for ln in lines:
        empty = (not ln[1])
        if empty and (not merged or not merged[-1][1]):
            continue
        merged.append(ln)
    while merged and not merged[-1][1]:
        merged.pop()
    return merged


# ─────────────────────── 3. 渲染：行 → 終端 ───────────────────────
C = {'reset': '\033[0m', 'b': '\033[1m', 'd': '\033[2m',
     'i': '\033[3m', 'u': '\033[4m', 'cy': '\033[36m', 'bl': '\033[34m'}


def render_line(pref, segs, kind, use_color):
    pieces = []  # [(文字, 樣式碼)]

    def push(t, st):
        if not t:
            return
        if pieces and pieces[-1][1] == st:
            pieces[-1] = (pieces[-1][0] + t, st)
        else:
            pieces.append((t, st))

    if pref:
        push(pref, '')
    for text, run in segs:
        if run is None:
            push(text, C['d'] if use_color and kind in ('rule', 'media', 'media-url', 'img') else '')
            continue
        st = ''
        if use_color:
            if kind in HEADINGS:
                st += C['b']
            if kind == 'quote':
                st += C['d']
            if run.code:
                st += C['cy']
            if run.href:
                st += C['u'] + C['bl']
            elif run.b and kind not in HEADINGS:
                st += C['b']
            if run.i:
                st += C['i']
        push(text, st)
    return ''.join((st + t + C['reset']) if st else t for t, st in pieces)


# ─────────────────────────── 4. CLI ───────────────────────────
def fetch(url, timeout=8):
    req = Request(url, headers={'User-Agent': 'minireader/0.1 (+poc)'})
    with urlopen(req, timeout=timeout) as resp:
        raw = resp.read(2_000_000)
        cs = resp.headers.get_content_charset() or 'utf-8'
    try:
        return raw.decode(cs, 'replace')
    except LookupError:
        return raw.decode('utf-8', 'replace')


def load(src, timeout=8):
    """回傳 (html, base_url)。src 可以是 URL 或本地檔。"""
    if re.match(r'^https?://', src):
        return fetch(src, timeout), src
    with open(src, encoding='utf-8') as f:
        return f.read(), ''


def parse(html, base='', drop_chrome=False):
    f = BlockFilter(base=base, drop_chrome=drop_chrome)
    f.feed(html)
    f.flush()
    return f


def render(f, width, color=True, with_links=True):
    lines = layout(f.blocks, width)
    body = '\n'.join(render_line(p, s, k, color) for p, s, k in lines)
    if with_links and f.links:
        body += '\n\n' + '─' * width + '\n連結 (%d)\n' % len(f.links)
        for i, u in enumerate(f.links, 1):
            body += '  %2d. %s\n' % (i, u)
    return body.rstrip('\n'), lines


def to_doc(f, width, color=False):
    """把版面序列化成 display list（給任何前端吃）。相鄰同樣式的片段會合併。"""
    lines = layout(f.blocks, width)
    out = []
    for pref, segs, kind in lines:
        seg_out = []
        for t, r in segs:
            if r is None:
                item = {'t': t, 'n': 1}
            else:
                item = {'t': t, 'b': bool(r.b), 'i': bool(r.i),
                        'c': bool(r.code), 'h': r.href}
            if seg_out and not item.get('n') and not seg_out[-1].get('n'):
                prev = seg_out[-1]
                if all(prev.get(k) == item.get(k) for k in ('b', 'i', 'c', 'h')):
                    prev['t'] += item['t']
                    continue
            seg_out.append(item)
        out.append({'p': pref, 'k': kind, 's': seg_out})
    return {
        'v': 1,
        'title': f.title.strip(),
        'width': width,
        'lines': out,
        'links': f.links,
        'media': [{'tag': t, 'url': u} for t, u in f.media],
    }


def from_doc(doc):
    """display list → (lines, links)，讓前端不必懂 HTML。"""
    lines = []
    for ln in doc['lines']:
        segs = []
        for s in ln['s']:
            if s.get('n'):
                segs.append((s['t'], None))
            else:
                segs.append((s['t'], Run(s['t'], s['b'], s['i'], s['c'], s.get('h'))))
        lines.append([ln['p'], segs, ln['k']])
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('src', help='HTML 檔或 http(s) URL')
    ap.add_argument('--width', type=int, default=76)
    ap.add_argument('--no-color', action='store_true')
    ap.add_argument('--no-chrome', action='store_true', help='剝離 nav/header/footer/aside')
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('--emit', action='store_true', help='輸出 display list（JSON）給前端')
    args = ap.parse_args(argv)

    t0 = time.time()
    html, base = load(args.src)
    t1 = time.time()

    f = parse(html, base, args.no_chrome)

    if args.emit:
        import json
        print(json.dumps(to_doc(f, args.width), ensure_ascii=False))
        return

    body, lines = render(f, args.width, not args.no_color)
    print(body)

    if args.stats:
        from collections import Counter
        c = Counter(b.kind for b in f.blocks)
        txt = ''.join(r.text for b in f.blocks for r in b.runs)
        el = (time.time() - t1) * 1000
        sys.stderr.write('\n── stats ──\n')
        sys.stderr.write('抓取 %.0f ms / 過濾＋排版 %.1f ms\n' % ((t1 - t0) * 1000, el))
        sys.stderr.write('HTML %d B → %d 塊 / %d 行 / %d 字元（%d 顯示欄）\n'
                         % (len(html), len(f.blocks), len(lines), len(txt), dw(txt)))
        sys.stderr.write('種類: %s\n' % ', '.join('%s=%d' % kv for kv in sorted(c.items(), key=lambda x: -x[1])))
        sys.stderr.write('媒體 %d / 連結 %d\n' % (len(f.media), len(f.links)))


if __name__ == '__main__':
    main()
