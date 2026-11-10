#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_resolver.py — 沙盒裡沒有 yt-dlp，用「注入假 runner」驗解析邏輯。

驗四件事：缺 yt-dlp 的降級、格式挑選、快取命中、簡介卡。
"""
import json
import os
import sys

import resolver

FIX = {
    'title': '測試影片：把瀏覽器拆成三段',
    'duration': 222,
    'uploader': '魚書館',
    'webpage_url': 'https://youtu.be/xyz',
    'formats': [
        {'format_id': '137', 'height': 1080, 'vcodec': 'avc1', 'acodec': 'none',
         'ext': 'mp4', 'url': 'https://cdn.example.com/v1080.mp4?sig=1'},
        {'format_id': '22', 'height': 720, 'vcodec': 'avc1', 'acodec': 'mp4a',
         'ext': 'mp4', 'url': 'https://cdn.example.com/v720.mp4?sig=1'},
        {'format_id': '18', 'height': 360, 'vcodec': 'avc1', 'acodec': 'mp4a',
         'ext': 'mp4', 'url': 'https://cdn.example.com/v360.mp4'},
    ],
}

ok = 0
total = 0


def check(name, cond, extra=''):
    global ok, total
    total += 1
    print('  %-28s %s %s' % (name, 'PASS' if cond else 'FAIL', extra))
    ok += 1 if cond else 0


class FakeOut:
    returncode = 0

    def __init__(self, payload):
        self.stdout = json.dumps(payload).encode()


def main():
    resolver.CACHE_PATH = '/tmp/t_resolve.json'
    if os.path.exists(resolver.CACHE_PATH):
        os.remove(resolver.CACHE_PATH)
    os.environ.pop('YTDLP_BIN', None)

    print('1. 沒有 yt-dlp 時要降級，不能炸')
    real_bin = resolver.ytdlp_bin
    resolver.ytdlp_bin = lambda: None          # 假裝機器上沒裝
    check('have_ytdlp=False', resolver.have_ytdlp() is False)
    check('resolve() → None', resolver.resolve('https://youtu.be/x', use_cache=False) is None)
    resolver.ytdlp_bin = real_bin

    print('2. 格式挑選：1080 無聲要排除，720 有聲優先')
    info = resolver.normalize(FIX)
    check('挑到 720', info and info['height'] == 720, info['stream'][-24:] if info else '')
    check('直鏈正確', info['stream'].startswith('https://cdn.example.com/v720'))

    print('2b. B 站分軌：沒有 ffmpeg 走單檔，有 ffmpeg 才挑分軌')
    DASH = {
        'title': 'B 站分軌測試', 'duration': 60,
        'formats': [
            {'height': 1080, 'vcodec': 'avc1', 'acodec': 'none', 'ext': 'mp4',
             'url': 'https://cdn.example.com/bili_v1080.m4s'},
            {'height': 360, 'vcodec': 'avc1', 'acodec': 'mp4a', 'ext': 'mp4',
             'url': 'https://cdn.example.com/bili_p360.mp4'},
            {'vcodec': 'none', 'acodec': 'mp4a', 'ext': 'm4a', 'abr': 132,
             'url': 'https://cdn.example.com/bili_a132.m4a'},
        ],
    }
    pro = resolver.normalize(DASH, prefer_progressive=True)
    check('無 ffmpeg → 360 單檔', pro['height'] == 360 and pro['audio'] is None)
    dash = resolver.normalize(DASH, prefer_progressive=False)
    check('有 ffmpeg → 1080 分軌', dash['height'] == 1080 and bool(dash['audio']))
    check('分軌標記 split', dash['split'] is True)

    print('3. 有 yt-dlp 時走 subprocess（注入假 runner）')
    calls = []

    def fake_run(cmd, **kw):
        calls.append(cmd)
        return FakeOut(FIX)

    resolver.subprocess.run = fake_run
    os.environ['YTDLP_BIN'] = '/bin/sh'
    got = resolver.resolve('https://youtu.be/xyz')
    check('拿到簡介', bool(got), got['title'] if got else '')
    check('命令帶 -j', '-j' in calls[0])
    check('命令帶 --', calls[0][-2] == '--')

    print('4. 快取：第二次即使解析器壞掉也要命中')
    def boom(cmd, **kw):
        raise TimeoutError('模擬超時')

    resolver.subprocess.run = boom
    got2 = resolver.resolve('https://youtu.be/xyz')
    check('命中快取', bool(got2) and got2['stream'] == got['stream'])
    check('快取檔存在', os.path.exists(resolver.CACHE_PATH))

    print('5. 簡介卡')
    card = resolver.card(got, width=60)
    print('\n'.join('     ' + l for l in card.split('\n')))
    check('卡上有時長', '03:42' in card)
    check('卡上有 720p', '720p' in card)

    print('\n%d/%d 通過' % (ok, total))
    return 0 if ok == total else 1


if __name__ == '__main__':
    sys.exit(main())
