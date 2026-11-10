#!/usr/bin/env python3
"""从 GNU Unifont .hex 烘 MRPF ROM（1bpp，8/16px 宽）"""
import struct, sys

src = '/tmp/unifont.hex'
path = sys.argv[1] if len(sys.argv) > 1 else '/home/user17/mrp2-bench/assets/font16.rom'

want = set(range(0x20, 0x7F))
want |= set(range(0x3000, 0x3040))
want |= set(range(0x2190, 0x2200))
want |= set(range(0x25A0, 0x2600))
want |= set(range(0xFF00, 0xFFF0))
# GB2312 全字符集
for hi in range(0xA1, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            want.add(ord(bytes([hi, lo]).decode('gb2312')))
        except Exception:
            pass
scene = ("1 MPR2 tty2 CPU MEM DISK SYSTEM/SPEC HOST ARTIX-BZYS17MINTSTAR OS LINUX OPENRC "
         "KERNEL i7-6700 4C/8T GPU RTX 2060 HD 530 15.5 118 GB UPTIME 04:12 foot user17@artix "
         "fastfetch Artix Linux x86_64 Host Precision Tower 3620 Kernel WM sway 1.12 Wayland "
         "Memory GiB Terminal 1.28.0 Desktop Music Pictures Videos Firefox 网易云音乐 "
         "https://music.163.com 发现音乐 我的音乐 正在播放 Something Just Like This 播放列表 "
         "每日推荐 私人 FM QQ 糖猫大家庭 DSH 桌宠交流群 卡拉彼丘交流群 命运扳机官方交流群 "
         "Cakumi 内测群 PvP 领域大神 北冥 25食安中外2班 群公告 全新的策划栏目「策划面对面」"
         "正式启动，欢迎留言提问。FIREFOX TERMINAL FILES MONITOR EDITOR CHERRY WIFI ▶ · — →")
want |= {ord(c) for c in scene}

glyphs = []
for line in open(src):
    line = line.strip()
    if not line or ':' not in line:
        continue
    cp_s, hx = line.split(':', 1)
    cp = int(cp_s, 16)
    if cp not in want:
        continue
    if len(hx) % 16:
        continue
    rw = len(hx) // 16          # 每行 hex 字符数
    width = rw * 4
    pitch = (width + 7) // 8
    rows = [int(hx[i*rw:(i+1)*rw], 16) for i in range(16)]
    if width == 16:
        data = b''.join(struct.pack('>H', r) for r in rows)
    elif width == 8:
        data = bytes(rows)
    else:
        # 32px 双宽等少见：裁成 16px
        continue
    glyphs.append((cp, width, pitch, data))
glyphs.sort()
cps = [g[0] for g in glyphs]
data_off = 14 + len(cps) * 12
out = bytearray(b'MRPF' + struct.pack('<HI', 1, len(cps)) + struct.pack('<I', data_off))
data = bytearray()
for cp, w, pitch, buf in glyphs:
    out += struct.pack('<IIBBBB', cp, len(data), w, 16, pitch, 0)
    data += buf
out += data
open(path, 'wb').write(out)
print('字形 %d → %.1f KB → %s' % (len(cps), len(out)/1024, path))
