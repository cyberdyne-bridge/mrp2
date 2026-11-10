#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""resolver.py — 把「頁面 URL」解析成「可播直鏈 + 簡介」（包 yt-dlp）

為什麼要這一層：真實站點的 <video src> 常常是 blob:（JS 現場組的）、
帶簽名的 .m3u8，或者乾脆沒有 src（YouTube／B站 全靠 JS）。
這一層只做兩件事：拿到直鏈、拿到簡介，其餘交給 mpv。

  python3 resolver.py https://youtu.be/xxxx     # 印簡介卡
  python3 resolver.py --json https://...        # 原始 JSON
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time

CACHE_PATH = '/tmp/minireader_resolve.json'
TTL = 3600

MEDIA_EXT = ('.mp4', '.m4v', '.webm', '.mkv', '.mov', '.mp3',
             '.m4a', '.ogg', '.opus', '.flac', '.wav', '.aac')
STREAM_EXT = ('.m3u8', '.mpd')


def ytdlp_bin():
    """允許用 YTDLP_BIN 指定（測試時塞假貨）。"""
    return os.environ.get('YTDLP_BIN') or shutil.which('yt-dlp')


def have_ytdlp():
    return bool(ytdlp_bin())


def _path(url):
    return url.split('?')[0].split('#')[0].lower()


def is_direct(url):
    """看起來就是一個能吃進 ffmpeg 的檔案。"""
    return _path(url).endswith(MEDIA_EXT)


def needs_resolve(url):
    """HLS/DASH/blob/沒有副檔名 → 都可能不是直鏈。"""
    p = _path(url)
    return p.endswith(STREAM_EXT) or url.startswith('blob:') or not p.endswith(MEDIA_EXT)


# ─────────────────────── 快取 ───────────────────────
def _load_cache():
    try:
        with open(CACHE_PATH, encoding='utf-8') as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def _save_cache(c):
    try:
        with open(CACHE_PATH, 'w', encoding='utf-8') as f:
            json.dump(c, f, ensure_ascii=False)
    except Exception:
        pass


# ─────────────────────── 解析 ───────────────────────
def have_ffmpeg():
    return shutil.which('ffmpeg') is not None


def resolve(url, timeout=25, use_cache=True):
    """回傳 dict（title/duration/uploader/stream/height/audio）或 None。"""
    import bili
    if bili.is_bili(url):
        url = bili.canonical(url) or url

    cache = _load_cache() if use_cache else {}
    hit = cache.get(url)
    if use_cache and hit and time.time() - hit.get('ts', 0) < TTL:
        return hit.get('info')

    if not have_ytdlp():
        return None

    cmd = [ytdlp_bin(), '-j', '--no-playlist', '--no-warnings',
           '--socket-timeout', '10']
    ck = os.environ.get('YTDLP_COOKIES')
    if ck:
        if ck.startswith('browser:'):
            cmd += ['--cookies-from-browser', ck.split(':', 1)[1]]
        else:
            cmd += ['--cookies', ck]
    cmd += ['--', url]
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=timeout).stdout
        info = json.loads(out.decode('utf-8', 'replace'))
    except Exception:
        return None

    got = normalize(info, url, prefer_progressive=not have_ffmpeg())
    if got:
        cache[url] = {'ts': time.time(), 'info': got}
        _save_cache(cache)
    return got


def normalize(info, fallback_url='', prefer_progressive=True):
    """從 yt-dlp 的 -j 輸出裡挑格式。

    prefer_progressive=True：優先「有聲有影」的單檔（B 站 360/480/720 的 durl），
    沒有 ffmpeg 也能直接播。
    否則允許挑分軌，並附上 audio 直鏈讓 mpv 用 --audio-file 掛第二軌。
    """
    fmts = [f for f in (info.get('formats') or []) if f.get('url')]
    if not fmts:
        if info.get('url'):
            fmts = [{'url': info['url'], 'height': info.get('height') or 0}]
        else:
            return None

    def h(f):
        return f.get('height') or 0

    def has_v(f):
        return f.get('vcodec') not in (None, 'none')

    def has_a(f):
        return f.get('acodec') not in (None, 'none')

    both = [f for f in fmts if has_v(f) and has_a(f)]
    vonly = [f for f in fmts if has_v(f) and not has_a(f)]
    aonly = [f for f in fmts if not has_v(f) and has_a(f)]

    if prefer_progressive and both:
        cand = both
    elif both or vonly:
        cand = both + vonly
    else:
        cand = fmts

    under = [f for f in cand if 0 < h(f) <= 1080]
    best = max(under, key=h) if under else min(cand, key=h)

    audio = None
    if not has_a(best) and aonly:
        audio = max(aonly, key=lambda f: f.get('abr') or f.get('tbr') or 0).get('url')

    return {
        'title': (info.get('title') or '').strip(),
        'duration': int(info.get('duration') or 0),
        'uploader': (info.get('uploader') or info.get('channel') or '').strip(),
        'thumb': info.get('thumbnail') or '',
        'stream': best.get('url') or '',
        'height': h(best),
        'ext': best.get('ext') or '',
        'audio': audio,
        'split': bool(audio),
        'page': info.get('webpage_url') or fallback_url,
    }


# ─────────────────────── 簡介卡 ───────────────────────
def mmss(sec):
    sec = int(sec or 0)
    if not sec:
        return '--:--'
    h, m, s = sec // 3600, (sec % 3600) // 60, sec % 60
    return ('%d:%02d:%02d' % (h, m, s)) if h else ('%02d:%02d' % (m, s))


def _dw(s):
    import unicodedata
    return sum(2 if unicodedata.east_asian_width(c) in 'WF' else
               0 if unicodedata.combining(c) else 1 for c in s)


def card(info, width=64):
    if not info:
        return '（解析不出來：沒有 yt-dlp，或這條連結不吃）'
    head = '影片 · %s%s' % (mmss(info['duration']),
                           (' · %dp' % info['height']) if info['height'] else '')
    lines = ['┌ %s %s' % (head, '─' * max(1, width - _dw(head) - 3))]
    if info['title']:
        lines.append('│ %s' % info['title'][:width - 3])
    if info['uploader']:
        lines.append('│ 來源：%s' % info['uploader'][:width - 5])
    host = re.sub(r'^https?://([^/]+).*', r'\1', info['stream'])
    lines.append('│ 直鏈：%s（%s%s）' % (host[:width - 16], info['ext'] or '?',
                                      '，分軌' if info.get('split') else ''))
    lines.append('│ ↳ 播放：mpv --geometry=360x203 -- \'<直鏈>\'')
    lines.append('└' + '─' * max(0, width - 1))
    return '\n'.join(lines)


def main(argv=None):
    argv = argv or sys.argv[1:]
    as_json = '--json' in argv
    argv = [a for a in argv if a != '--json']
    if not argv:
        print(__doc__)
        return 2
    url = argv[0]
    if not have_ytdlp():
        print('這台機器沒有 yt-dlp 喵')
        print('直鏈判定：%s' % ('是（直接給 mpv）' if is_direct(url) else '否（需要解析）'))
        return 1
    info = resolve(url)
    if as_json:
        print(json.dumps(info, ensure_ascii=False, indent=2))
    else:
        print(card(info))
    return 0 if info else 1


if __name__ == '__main__':
    sys.exit(main())
