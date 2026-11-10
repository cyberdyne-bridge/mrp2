import importlib.util, struct
spec = importlib.util.spec_from_file_location("mkfont", "/home/user17/mrp2/mkfont.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
m.FONT = '/usr/share/fonts/Unifont/Unifont.ttf'
text = ("MRP2 DESKTOP   ·   120 Hz   ·   tty2"
 "Reader — sample.html"
 "魚書館  ·  首頁  目錄  關於"
 "把瀏覽器拆成三段：抓頁、過濾、排版。"
 "合成器只收「有墨」的地方，黑底不上線。"
 "影片走 PLANE，文字走 TEXT / SPANS。"
 "→  開啟文章" "→  播放影片"
 "DAMAGE 只重畫變化的一條，其餘不動。"
 "影片 · 1080p · 120Hz"
 "NV12 直掃 · 硬體 CSC"
 "▶  播放中   00:12 / 00:12")
chars = set(chr(c) for c in range(0x20,0x7F))
chars |= set('，。、；：！？「」『』（）《》…—·→▶')
chars |= set(text)
for i in range(500):
    try: chars.add(bytes([0xB0+i//94, 0xA1+i%94]).decode('gb2312'))
    except Exception: pass
chars = {c for c in chars if c.strip()}
g = m.build(chars)
cps = sorted(g)
index_size = len(cps)*12
data_off = 14 + index_size          # 修正：頭是 14 不是 12
out = bytearray(b'MRPF' + struct.pack('<HI',1,len(cps)) + struct.pack('<I',data_off))
data = bytearray()
for cp in cps:
    w,h,pitch,buf = g[cp]
    out += struct.pack('<IIBBBB', cp, len(data), w, h, pitch, 0)
    data += buf
out += data
open('/tmp/font_uni.rom','wb').write(out)
print('字形 %d → %.1f KB (data_off=%d)' % (len(cps), len(out)/1024, data_off))
