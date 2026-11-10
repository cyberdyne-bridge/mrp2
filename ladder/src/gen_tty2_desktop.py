#!/usr/bin/env python3
"""生成 tty2 桌面场景（复刻 tty1：顶栏 + HUD + dock + 终端/浏览器/QQ）"""
import proto, sys

W, H = 1920, 1080
ops = [
    ('surf', 90,    0,   0, 1920, 1080, 0, 0, 1, 0, 0),   # 桌布
    ('surf', 91,    0,   0, 1920,   34, 1, 2, 1, 2, 0),   # 顶栏
    ('surf', 92,   40,  64,  430,  330, 2, 1, 1, 1, 0),   # HUD 面板
    ('surf', 93,   40, 980,  600,   76, 3, 1, 1, 1, 0),   # dock
    ('surf', 94,  520,  70,  780,  560, 4, 1, 1, 1, 0),   # 终端
    ('surf', 95,  520, 660,  780,  360, 5, 1, 1, 1, 0),   # 浏览器
    ('surf', 96, 1340,  70,  540,  900, 6, 1, 1, 1, 0),   # QQ
    ('surf', 98,   40, 430,  430,  520, 7, 1, 1, 1, 0),   # Cherry Studio
    ('surf', 97,    0,   0, 1920, 1080, 9, 1, 0, 2, 0),   # 文字层
]
T = []
def t(x, y, c, s): T.append((97, x, y, c, s))

# 顶栏
t(20, 8, 0, "1  MPR2 · tty2      CPU 12%    MEM 3.2G    DISK 16G    10:24")
# HUD
t(60,  76, 2, "SYSTEM / SPEC")
t(60, 108, 0, "HOST    ARTIX-BZYS17MINTSTAR")
t(60, 130, 0, "OS      ARTIX LINUX · OPENRC")
t(60, 152, 0, "KERNEL  7.2.14-artix1-1")
t(60, 174, 0, "CPU     i7-6700 · 4C/8T")
t(60, 196, 0, "GPU     RTX 2060 + HD 530")
t(60, 218, 0, "MEM     3.2 / 15.5 GB")
t(60, 240, 0, "DISK    15.5 / 118 GB")
t(60, 262, 0, "UPTIME  04:12")
# 终端
t(540,  76, 3, "foot — user17@artix")
t(540, 106, 0, "user17@artix ~")
t(540, 132, 1, "$ fastfetch")
t(540, 160, 0, "OS       Artix Linux x86_64")
t(540, 182, 0, "Host     Precision Tower 3620")
t(540, 204, 0, "Kernel   7.2.14-artix1-1")
t(540, 226, 0, "WM       sway 1.12 (Wayland)")
t(540, 248, 0, "CPU      i7-6700 (8) @ 4.00GHz")
t(540, 270, 0, "GPU      NVIDIA RTX 2060")
t(540, 292, 0, "Memory   3.2 GiB / 15.5 GiB")
t(540, 314, 0, "Terminal foot 1.28.0")
t(540, 350, 1, "$ ls")
t(540, 374, 0, "Desktop   Music   Pictures   Videos")
# 浏览器
t(540, 666, 3, "Firefox — 网易云音乐")
t(540, 696, 1, "https://music.163.com")
t(540, 724, 0, "网易云音乐   ·   发现音乐   ·   我的音乐")
t(540, 752, 2, "▶  正在播放：Something Just Like This")
t(540, 780, 0, "播放列表 · 每日推荐 · 私人 FM")
# QQ
t(1360, 76, 3, "QQ")
t(1360, 108, 2, "糖猫大家庭 (97)")
t(1360, 140, 0, "DSH 桌宠交流群")
t(1360, 172, 0, "卡拉彼丘交流群")
t(1360, 204, 0, "命运扳机官方交流群")
t(1360, 236, 0, "Cakumi 内测群")
t(1360, 268, 0, "PvP 领域大神")
t(1360, 300, 0, "北冥 MMR")
t(1360, 332, 0, "25食安中外2班")
t(1360, 380, 1, "群公告")
t(1360, 404, 0, "全新的策划栏目「策划面对面」")
t(1360, 426, 0, "正式启动，欢迎留言提问。")
# Cherry Studio
t(60, 442, 3, "Cherry Studio")
t(60, 474, 2, "Claude Code")
t(60, 506, 0, "> 帮我看看 tty2 的渲染")
t(60, 528, 0, "  桌面已复刻完成，")
t(60, 550, 0, "  游标移动只重绘局部，")
t(60, 572, 0, "  静止 3 秒自动定格。")
t(60, 604, 1, "● 正在运行 mrpd_kms")
t(60, 626, 0, "  120 Hz · 0 漏拍")
t(60, 670, 0, "会话")
t(60, 692, 0, "  tty2 桌面渲染")
t(60, 714, 0, "  输入法修复")
t(60, 736, 0, "  QQ 安装")
# dock
t(60, 1002, 0, "FIREFOX    TERMINAL    QQ    FILES    MONITOR    EDITOR    CHERRY    WIFI")

for x in T:
    ops.append(('text',) + x)
ops.append(('commit', 97))
out = sys.argv[1] if len(sys.argv) > 1 else 'assets/tty2desktop.wire'
open(out, 'wb').write(proto.encode(ops))
print("wrote", out, len(proto.encode(ops)), "bytes,", len(T), "texts")
