#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""proto.py — MRP2：系統級渲染協議（v2）

跟 X11 / Wayland 平級的意思是：不只我們的閱讀器，桌布、面板、播放器、
任何 App 都能接；協議本身負責 surface 生命週期、輸入分發、幀回呼。

v2 = v1 的超集：
  · HELLO / HELLO_ACK 握手與能力協商
  · role（background / toplevel / panel / overlay / popup）
  · server → client 事件（KEY / POINTER / FOCUS / FRAME / CONFIGURE / ERROR）
  · v1 封包仍可解（magic 'MRP1'）

封包：magic(4) ver(1) flags(1) pad(1) nops(4)
op  ：type(1) len(2) payload
"""
import struct

MAGIC2 = b'MRP2'
MAGIC1 = b'MRP1'
VERSION = 2

# ── client → server ──
OP_SURF, OP_DAMAGE, OP_SPANS, OP_COMMIT, OP_UNMAP = 1, 2, 3, 4, 5
OP_HELLO, OP_FRAME_ACK, OP_BUFFER, OP_PLANE, OP_TEXT = 6, 7, 8, 9, 10

# ── server → client ──
EV_HELLO_ACK, EV_KEY, EV_POINTER, EV_FOCUS = 0x81, 0x82, 0x83, 0x84
EV_FRAME, EV_CONFIGURE, EV_ERROR = 0x85, 0x86, 0x87

ROLES = {'background': 0, 'toplevel': 1, 'panel': 2, 'overlay': 3, 'popup': 4}
ROLE_NAMES = {v: k for k, v in ROLES.items()}

CAP_SPANS = 1
CAP_SHM = 2
CAP_DMABUF = 4
CAP_FRAME_CALLBACK = 8

PALETTE = [
    (0x2B, 0x2B, 0x2B), (0x1A, 0x5F, 0xD0), (0xA8, 0x5A, 0x00),
    (0x10, 0x10, 0x10), (0x8A, 0x8A, 0x86), (0x77, 0x7B, 0x82),
]

HDR = struct.Struct('<4sBBBI')
SURF = struct.Struct('<HhhHHhBBBH')      # + role, parent
SURF1 = struct.Struct('<HhhHHhBB')       # v1 相容
DAMAGE = struct.Struct('<HhhHH')
SPANS = struct.Struct('<HHHH')
SPAN = struct.Struct('<HHHB')
U16 = struct.Struct('<H')

HELLO = struct.Struct('<BI')             # proto, caps
FRAME_ACK = struct.Struct('<HI')         # surface, serial
HELLO_ACK = struct.Struct('<BIIHHh')     # proto, caps, serial, w, h, refresh
KEY = struct.Struct('<HIIBB')            # surface, serial, keysym, state, mods
POINTER = struct.Struct('<HhhBBI')       # surface, x, y, button, state, serial
FOCUS = struct.Struct('<HB')             # surface, state
BUFFER = struct.Struct('<HHBHH')         # id, tile_y, fmt, w, h
PLANE = struct.Struct('<HhhHHH')          # id, x, y, w, h, stride
TEXT = struct.Struct('<HhhBH')            # id, x, y, color, len


def _op(typ, payload):
    return struct.pack('<BH', typ, len(payload)) + payload


def encode(ops):
    body, n = [], 0
    for op in ops:
        t = op[0]
        if t == 'surf':
            _, sid, x, y, w, h, z, layer, flags, role, parent = op
            body.append(_op(OP_SURF, SURF.pack(sid, x, y, w, h, z, layer, flags, role, parent)))
        elif t == 'damage':
            body.append(_op(OP_DAMAGE, DAMAGE.pack(*op[1:])))
        elif t == 'spans':
            _, sid, tx, ty, spans = op
            payload = SPANS.pack(sid, tx, ty, len(spans))
            payload += b''.join(SPAN.pack(*s) for s in spans)
            body.append(_op(OP_SPANS, payload))
        elif t == 'commit':
            body.append(_op(OP_COMMIT, U16.pack(op[1])))
        elif t == 'unmap':
            body.append(_op(OP_UNMAP, U16.pack(op[1])))
        elif t == 'hello':
            body.append(_op(OP_HELLO, HELLO.pack(op[1], op[2])))
        elif t == 'frame_ack':
            body.append(_op(OP_FRAME_ACK, FRAME_ACK.pack(op[1], op[2])))
        elif t == 'text':
            _, sid, x, y, color, text = op
            raw = text.encode('utf-8')
            body.append(_op(OP_TEXT, TEXT.pack(sid, x, y, color, len(raw)) + raw))
        elif t == 'plane':
            _, sid, x, y, w, h, stride = op
            body.append(_op(OP_PLANE, PLANE.pack(sid, x, y, w, h, stride)))
        elif t == 'buffer':
            _, sid, ty, fmt, w, h, data = op
            body.append(_op(OP_BUFFER, BUFFER.pack(sid, ty, fmt, w, h) + data))
        else:
            raise ValueError('未知 op：%r' % (t,))
        n += 1
    return HDR.pack(MAGIC2, VERSION, 0, 0, n) + b''.join(body)


def encode_events(evs):
    """server → client 也用同一種封包，方便同一條 socket 雙向跑。"""
    body, n = [], 0
    for ev in evs:
        t = ev[0]
        if t == 'hello_ack':
            body.append(_op(EV_HELLO_ACK, HELLO_ACK.pack(*ev[1:])))
        elif t == 'key':
            body.append(_op(EV_KEY, KEY.pack(*ev[1:])))
        elif t == 'pointer':
            body.append(_op(EV_POINTER, POINTER.pack(*ev[1:])))
        elif t == 'focus':
            body.append(_op(EV_FOCUS, FOCUS.pack(*ev[1:])))
        elif t == 'frame':
            body.append(_op(EV_FRAME, FRAME_ACK.pack(*ev[1:])))
        elif t == 'configure':
            body.append(_op(EV_CONFIGURE, struct.pack('<HHHI', *ev[1:])))
        elif t == 'error':
            code, msg = ev[1], ev[2]
            body.append(_op(EV_ERROR, struct.pack('<H', code) + msg.encode('utf-8')))
        else:
            raise ValueError('未知 event：%r' % (t,))
        n += 1
    return HDR.pack(MAGIC2, VERSION, 0, 0, n) + b''.join(body)


def decode(data):
    """回傳 ops / events 的 tuple 清單；v1 封包一併支援。"""
    magic, ver, _, _, nops = HDR.unpack_from(data, 0)
    if magic not in (MAGIC1, MAGIC2):
        raise ValueError('magic 不對：%r' % magic)
    if magic == MAGIC2 and ver != 2:
        raise ValueError('版本不支援：%d' % ver)
    legacy = (magic == MAGIC1)
    ops, off = [], HDR.size
    for _ in range(nops):
        typ, ln = struct.unpack_from('<BH', data, off)
        off += 3
        p = data[off:off + ln]
        off += ln
        if typ == OP_SURF:
            if legacy:
                sid, x, y, w, h, z, layer, flags = SURF1.unpack(p[:14])
                ops.append(('surf', sid, x, y, w, h, z, layer, flags, 1, 0))
            else:
                ops.append(('surf',) + SURF.unpack(p))
        elif typ == OP_DAMAGE:
            ops.append(('damage',) + DAMAGE.unpack(p))
        elif typ == OP_SPANS:
            sid, tx, ty, n = SPANS.unpack_from(p, 0)
            sp, q = [], SPANS.size
            for _ in range(n):
                sp.append(SPAN.unpack_from(p, q))
                q += SPAN.size
            ops.append(('spans', sid, tx, ty, sp))
        elif typ == OP_COMMIT:
            ops.append(('commit',) + U16.unpack(p))
        elif typ == OP_UNMAP:
            ops.append(('unmap',) + U16.unpack(p))
        elif typ == OP_HELLO:
            ops.append(('hello',) + HELLO.unpack(p))
        elif typ == OP_FRAME_ACK:
            ops.append(('frame_ack',) + FRAME_ACK.unpack(p))
        elif typ == OP_TEXT:
            sid, x, y, color, ln2 = TEXT.unpack_from(p, 0)
            ops.append(('text', sid, x, y, color, p[TEXT.size:TEXT.size + ln2].decode('utf-8', 'replace')))
        elif typ == OP_PLANE:
            ops.append(('plane',) + PLANE.unpack(p))
        elif typ == OP_BUFFER:
            sid, ty, fmt, w, h = BUFFER.unpack_from(p, 0)
            ops.append(('buffer', sid, ty, fmt, w, h, p[BUFFER.size:]))
        elif typ == EV_HELLO_ACK:
            ops.append(('hello_ack',) + HELLO_ACK.unpack(p))
        elif typ == EV_KEY:
            ops.append(('key',) + KEY.unpack(p))
        elif typ == EV_POINTER:
            ops.append(('pointer',) + POINTER.unpack(p))
        elif typ == EV_FOCUS:
            ops.append(('focus',) + FOCUS.unpack(p))
        elif typ == EV_FRAME:
            ops.append(('frame',) + FRAME_ACK.unpack(p))
        elif typ == EV_CONFIGURE:
            ops.append(('configure',) + struct.unpack('<HHHI', p))
        elif typ == EV_ERROR:
            code = struct.unpack_from('<H', p, 0)[0]
            ops.append(('error', code, p[2:].decode('utf-8', 'replace')))
        else:
            continue                                  # 未知 op：跳過（向前兼容）
    return ops
