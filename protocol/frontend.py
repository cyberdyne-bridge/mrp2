#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""frontend.py — 只吃 display list 的前端（證明「協議」這一層真的獨立）

後端：minireader.py --emit → JSON（v1）
前端：本檔，不懂 HTML、不懂 CSS、不連網，只會畫。

  python3 minireader.py sample.html --emit --width 72 > /tmp/doc.json
  python3 frontend.py /tmp/doc.json            # 終端渲染
  python3 frontend.py /tmp/doc.json --dump     # 看協議本身

之後要換成 Wayland／GTK 前端，只要吃同一份 JSON。
"""
import json
import sys

import minireader as mr


def draw(doc, color=True):
    lines = mr.from_doc(doc)
    body = '\n'.join(mr.render_line(p, s, k, color) for p, s, k in lines)
    if doc.get('links'):
        body += '\n\n' + '─' * doc['width'] + '\n連結 (%d)\n' % len(doc['links'])
        for i, u in enumerate(doc['links'], 1):
            body += '  %2d. %s\n' % (i, u)
    return body.rstrip('\n')


def main(argv=None):
    argv = argv or sys.argv[1:]
    if not argv:
        print(__doc__)
        return 2
    path = argv[0]
    with open(path, encoding='utf-8') as f:
        doc = json.load(f)

    if '--dump' in argv:
        print('協議版本 v%s ｜ 寬 %d ｜ %d 行 ｜ 連結 %d ｜ 媒體 %d'
              % (doc['v'], doc['width'], len(doc['lines']),
                 len(doc['links']), len(doc['media'])))
        print(json.dumps(doc['lines'][:3], ensure_ascii=False, indent=1))
        return 0

    print(draw(doc, '--no-color' not in argv))
    return 0


if __name__ == '__main__':
    sys.exit(main())
