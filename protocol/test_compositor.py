#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_compositor.py — 合成決策核心的單元測試（不畫圖）"""
import sys

from compositor import Compositor, Rect, merge, subtract

ok = total = 0


def check(name, cond, extra=''):
    global ok, total
    total += 1
    print('  %-40s %s %s' % (name, 'PASS' if cond else 'FAIL', extra))
    ok += 1 if cond else 0


def scene(w=800, h=600):
    c = Compositor(w, h)
    a = c.add('a', 50, 50, 400, 300, (60, 60, 90), z=1)
    b = c.add('b', 300, 150, 400, 300, (90, 40, 50), z=2)
    return c, a, b


def main():
    print('1. 基礎：第一次有幀、第二次跳幀')
    c, a, b = scene()
    p1 = c.plan()
    p2 = c.plan()
    check('第一次 need_frame', p1['need_frame'])
    check('第二次跳幀', p2['need_frame'] is False and p2['pixels'] == 0)

    print('2. 拖窗：髒區要含新舊位置')
    c, a, b = scene()
    c.plan()
    c.move(b.id, 380, 240)
    p = c.plan()
    d = p['damage'][0]
    check('髒區從 x=300 起', d.x <= 300, d)
    check('髒區到 x=780 之後', d.x + d.w >= 780, d)
    check('合成像素 < 全屏', p['pixels'] < c.w * c.h)

    print('3. 不透明剔除：被 b 蓋住的 a 不該出現')
    c, a, b = scene()
    c.plan()
    c.damage(a.id, Rect(0, 0, 400, 300))
    p = c.plan()
    overlap = Rect(300, 150, 150, 200)   # a 與 b 的重疊區
    bad = [o for o in p['ops']
           if o['surface'] == a.id and o['rect'].intersect(overlap)]
    check('a 的重疊區被剔除', not bad, bad[:1])
    check('a 仍有可見部分', any(o['surface'] == a.id for o in p['ops']))

    print('4. 半透明不剔除下層')
    c, a, b = scene()
    b.opaque = False
    c.plan()
    c.damage(a.id, Rect(0, 0, 400, 300))
    p = c.plan()
    both = {o['surface'] for o in p['ops']}
    check('a、b 都出現', both == {a.id, b.id}, both)

    print('5. z 序：ops 由下往上')
    c, a, b = scene()
    c.plan()
    c.damage_rect(Rect(0, 0, 800, 600))
    p = c.plan()
    zs = [c.surfaces[o['surface']].z for o in p['ops']]
    check('z 非遞減', zs == sorted(zs), zs)

    print('6. direct scanout：單一全屏不透明')
    c = Compositor(800, 600)
    v = c.add('video', 0, 0, 800, 600, (10, 10, 10))
    p = c.plan()
    check('scanout 命中', p['scanout'] == v.id)
    check('零像素合成', p['pixels'] == 0 and p['bg'] == 0)

    print('7. 有游標層就不能直掃')
    c.add('cursor', 700, 300, 12, 12, (240, 240, 240), z=9)
    c.damage_rect(Rect(700, 300, 12, 12))
    p = c.plan()
    check('退回一般合成', p['scanout'] is None and p['pixels'] > 0)

    print('8. 幾何：subtract 面積守恆')
    r = Rect(0, 0, 100, 100)
    cut = Rect(20, 20, 30, 30)
    parts = subtract(r, cut)
    check('面積相加正確', sum(x.area() for x in parts) == r.area() - cut.area(),
          sum(x.area() for x in parts))
    check('切出四塊', len(parts) == 4, len(parts))

    print('9. merge 合併相鄰')
    m = merge([Rect(0, 0, 10, 10), Rect(0, 10, 10, 10)])
    check('併成一塊', len(m) == 1 and m[0].area() == 200, m)

    print('10. 點擊命中與焦點')
    c, a, b = scene()
    c.pointer_button(100, 100)      # 落在 a 上
    check('焦點 = a', c.focus == a.id, c.focus)
    c.pointer_button(500, 300)      # 落在 b 上
    check('焦點 = b', c.focus == b.id, c.focus)
    c.pointer_button(790, 590)      # 空白處
    check('空白處清焦點', c.focus is None, c.focus)

    print('11. layer：面板不搶鍵盤、命中優先')
    c, a, b = scene()
    panel = c.add('panel', 0, 0, 800, 30, (30, 30, 40), layer='top', keyboard=False)
    c.pointer_button(400, 10)
    check('點面板不搶焦點', c.focus is None, c.focus)
    check('命中面板', c.hit(400, 10).id == panel.id)
    check('面板在最上', c.stack()[-1].id == panel.id)

    print('12. 背景層不參與命中')
    c, a, b = scene()
    bg = c.add('wallpaper', 0, 0, 800, 600, (10, 10, 10), layer='background', input=False)
    check('命中視窗不是桌布', c.hit(100, 100).id == a.id)
    c.set_visible(a.id, False); c.set_visible(b.id, False)
    check('全空時不命中桌布', c.hit(100, 100) is None)

    print('13. alt-tab 走 MRU，再補其餘視窗')
    c, a, b = scene()
    c.pointer_button(100, 100)
    c.pointer_button(500, 300)
    check('MRU 第一是 b', c.focus_cycle(1) == a.id)
    check('再按回 b', c.focus_cycle(1) == b.id)

    print('14. 幀節流：同一格內的 damage 合併')
    from compositor import Scheduler
    c = Compositor(400, 300)
    c.add('w', 50, 50, 100, 80, (60, 60, 90))
    c.pending = []
    sch = Scheduler(60)
    frames = 0
    for t in range(0, 201):
        c.damage_rect(Rect(60, 60, 40, 40))
        if sch.poll(float(t), c):
            frames += 1
    check('200 ms / 60 Hz → 13 幀', frames == 13, frames)

    print('15. 背景層不妨礙直掃')
    c = Compositor(400, 300)
    c.add('wallpaper', 0, 0, 400, 300, (10, 10, 10), layer='background', input=False)
    v = c.add('video', 0, 0, 400, 300, (0, 0, 0))
    c.pending = [Rect(0, 0, 400, 300)]
    check('仍可直掃', c.plan()['scanout'] == v.id)

    print('16. 有 top 面板時不直掃')
    c.add('panel', 0, 0, 400, 24, (30, 30, 40), layer='top', keyboard=False)
    c.pending = [Rect(0, 0, 400, 300)]
    check('退回合成', c.plan()['scanout'] is None)

    print('\n%d/%d 通過' % (ok, total))
    return 0 if ok == total else 1


if __name__ == '__main__':
    sys.exit(main())
