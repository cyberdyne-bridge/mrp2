#!/usr/bin/env python3
"""全量 CJK 字体烘焙：ASCII + GB2312 6763 字 + 标点 → MRPF ROM"""
import importlib.util, struct, sys
spec = importlib.util.spec_from_file_location("mkfont", "/home/user17/mrp2/mkfont.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
m.FONT = '/usr/share/fonts/Unifont/Unifont.ttf'

chars = set(chr(c) for c in range(0x20, 0x7F))
chars |= set('，。、；：！？「」『』（）《》〈〉…—·→▶◀●○■□◆◇★☆ 　')
# GB2312 全字符集（符号区 + 一级/二级汉字）
for hi in range(0xA1, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            chars.add(bytes([hi, lo]).decode('gb2312'))
        except Exception:
            pass
chars = {c for c in chars if c.strip()}
g = m.build(chars)
cps = sorted(g)
index_size = len(cps) * 12
data_off = 14 + index_size          # 頭 14 字节
out = bytearray(b'MRPF' + struct.pack('<HI', 1, len(cps)) + struct.pack('<I', data_off))
data = bytearray()
for cp in cps:
    w, h, pitch, buf = g[cp]
    out += struct.pack('<IIBBBB', cp, len(data), w, h, pitch, 0)
    data += buf
out += data
path = sys.argv[1] if len(sys.argv) > 1 else '/home/user17/mrp2-bench/assets/font16.rom'
open(path, 'wb').write(out)
print('字形 %d → %.1f KB (data_off=%d) %s' % (len(cps), len(out)/1024, data_off, path))
