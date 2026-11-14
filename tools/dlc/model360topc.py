#!/usr/bin/env python3
"""model360topc.py: build the PC .model of a car from the console (Xbox 360, big-endian) .model.

  model360topc.py IN.model OUT.model [--expect PC.model]
  model360topc.py IN.model OUT.model --stream IN.model.stream OUT.model.stream [--expect PC.model PC.model.stream]
  model360topc.py IN.model OUT.model --no-deform

Standard library only, Python 3.8+. Reads the user's own files and writes new ones; nothing else is touched.
--expect compares the result with a real PC file and prints "identical" or the number of different bytes.

Deform streams (see below) are written for car body models only. By default the role comes from the file name:
<car>_XLOW/_LOW/_MID/_HIGH.model is a body; _FW_*/_BW_* (wheels) and the front-end model (<car>.model with a
.model.stream) are not. --deform / --no-deform override it.

Checked on 2026-11-12 against the Steam release: 473 of 475 base car models (bodies, wheels, all detail levels)
come out byte-identical; the two others differ in the PC data itself (one unused vertex zeroed on PC, one mesh with
two fewer vertices on PC). The three front-end model pairs tried (model + stream) are identical too.

Format, as far as the converter needs it (offsets in bytes, "word" = u32)
-------------------------------------------------------------------------
File
    0x00  u8 1, u8 0, u16 version 8                      (the u16 is swapped)
    PC only: u32 size of the deform block, then the deform block
    vertex declarations, section list, object size       (words, swapped)
    zero padding to a 16-byte file offset
    object (object size bytes, offsets inside are relative to its start)
    trailer: 5 words {1, 0, 0, data size, 0} then the data region
          or, for a streamed model, 2 words {1, 1}; the data region is then the .model.stream file after its
          12-byte header {0, size - 12, 0}
Vertex declarations: count, then per declaration {stride, 0, n, n x {stream, offset, D3DDECLTYPE, usage, index}}.
Section list: count, count x section offset, object size.
Object header: {VB count, VB records, index record count, index records, node count, nodes}
    node (0x40): {child count, children, 0, 0, bounds}; child (0x24): {entries, 0, -1, entry count, ..., hash, 0}
    entry (0x20): {0, 0, section, 0, part index, hash, 0, 0}
    section (0x50): {0x88888888, 0, 0, 0, bounds, first primitive record at +0x40}
    primitive record (0x10): {next primitive record or 0, index record, VB record, primitive type}
        console type 6 (triangle strip) becomes 4 (D3DPT_TRIANGLELIST)
    index record (0x10): console {index buffer header, index count, 0, 0}
                         PC      {0, list index count, 0, start * 8 | 2 if 32-bit indices}
        start counts 16-bit units from the start of the data region
    index buffer header (0x20): copied from the console unchanged (not swapped) except the word at +0x18 (offset of
        the strip in the data region), which is swapped
    VB record (0x1c): console {VB header, data offset, size, stride, count, 0, 0}; PC {0, new data offset, ...}
Every other object word is a plain 32-bit swap.
Data region, PC
    1. index lists, one per primitive record, in the order: sections in order of first use by the entries (nodes,
       children, entries), primitive records along each section's chain, then unused sections. Each console strip
       (u16, 0xFFFF = restart) becomes a list: per strip segment, triangle i is (a, b, c) for even i and (a, c, b)
       for odd i, degenerate triangles kept. Lists longer than 0xFFFF indices are written as u32.
    2. vertex buffers in the order of first use by the entries, then unused ones, packed at stride * count (no
       console padding), every 16-bit value swapped (all elements are FLOAT16_2 or FLOAT16_4).
    3. body models: the deform streams of every VB with stride >= 0x18, in order of first use: per vertex
       16 bytes (halves 0-6 of the vertex, 0), 8 bytes (halves 7-10), 8 bytes zero; vertex count rounded up to
       even when <= 512, else to a multiple of 1024 (per = 512, chunks = that / 512); the padding is zero.
Deform block (PC only): {unit count, 0x10 or 0, group count, offset of groups or 0}, units, groups.
    units (0x18 each), for every entry whose VB has deform streams, grouped by VB in stream order, then entry
        order: {part index, 0, 0, 2, 0, 0x10}, {part index, 0, 1, 2, 8, 0x10}
    groups (9 words): {VB index, per, chunks, size 1, offset 1, size 2, offset 2, size 3, offset 3}
"""
import argparse
import os
import re
import struct
import sys

BE = '>'
LE = '<'


def u32(b, o, e=BE):
    return struct.unpack_from(e + 'I', b, o)[0]


def locate(c):
    """(s, o, t): s = offset of the object-size word, o = object start, t = trailer offset."""
    n = len(c)
    for t in range(4, n - 0x14, 4):
        if u32(c, t + 0xc) == n - (t + 0x14):
            for s in range(4, t, 4):
                o = (s + 4 + 15) // 16 * 16
                if u32(c, s) == t - o:
                    return s, o, t
    t = n - 8  # streamed model: trailer {1, 1}
    if t > 0 and u32(c, t) == 1 and u32(c, t + 4) == 1:
        for s in range(4, t, 4):
            o = (s + 4 + 15) // 16 * 16
            if u32(c, s) == t - o:
                return s, o, t
    raise ValueError('layout not recognised (not a console car .model?)')


def swap32(b):
    n = len(b) // 4
    return struct.pack(LE + '%dI' % n, *struct.unpack(BE + '%dI' % n, b[:4 * n]))


def swap16(b):
    n = len(b) // 2
    return struct.pack(LE + '%dH' % n, *struct.unpack(BE + '%dH' % n, b[:2 * n]))


def strip_to_list(strip):
    out, seg = [], []
    for v in list(strip) + [0xFFFF]:
        if v == 0xFFFF:
            for i in range(len(seg) - 2):
                a, b, c = seg[i], seg[i + 1], seg[i + 2]
                out += (a, b, c) if i % 2 == 0 else (a, c, b)
            seg = []
        else:
            seg.append(v)
    return out


def convert(c, body=True, stream=None):
    """PC model bytes, or (model, stream) when the console .model.stream is given."""
    if c[:4] != b'\x01\x00\x00\x08':
        raise ValueError('not a console .model (header %s)' % c[:4].hex())
    s, o, t = locate(c)
    if stream is not None:
        if u32(stream, 4) != len(stream) - 12:
            raise ValueError('unexpected .model.stream header')
        data = stream[12:]
    else:
        data = c[t + 0x14:]

    def W(r):
        return u32(c, o + r)

    nvb, vbp, _nidx, _idxp, nnode, nodep = [W(4 * i) for i in range(6)]
    for nsec in range(1, 1000):
        if u32(c, s - 4 * (nsec + 1)) == nsec:
            break
    else:
        raise ValueError('section list not found')
    secs = list(struct.unpack_from(BE + '%dI' % nsec, c, s - 4 * nsec))

    obj = bytearray(swap32(c[o:t]))

    def put(r, v):
        struct.pack_into(LE + 'I', obj, r, v)

    def raw(r):
        obj[r:r + 4] = c[o + r:o + r + 4]

    vbs = []
    for i in range(nvb):
        r = vbp + 0x1c * i
        vbs.append(dict(rec=r, off=W(r + 4), stride=W(r + 12), count=W(r + 16)))
    vb_by_rec = {v['rec']: i for i, v in enumerate(vbs)}

    def chain(sec):
        prim, out = W(sec + 0x40), []
        while prim and len(out) < 4096:
            out.append(prim)
            prim = W(prim)
        return out

    order, ent_vb = [], []
    for k in range(nnode):
        nb = nodep + 0x40 * k
        nch, chp = W(nb), W(nb + 4)
        for j in range(nch):
            cr = chp + 0x24 * j
            ep, ne = W(cr), W(cr + 12)
            for q in range(ne):
                er = ep + 0x20 * q
                sec, part = W(er + 8), W(er + 16)
                for prim in chain(sec):
                    ent_vb.append((part, vb_by_rec[W(prim + 8)]))
                if sec not in order:
                    order.append(sec)
    order += [sec for sec in secs if sec not in order]

    # 1. index lists
    lists, pos = [], 0
    for prim in [p for sec in order for p in chain(sec)]:
        if W(prim + 12) != 6:
            raise ValueError('primitive type %d (only strips are known)' % W(prim + 12))
        put(prim + 12, 4)
        ir = W(prim + 4)
        hdr = W(ir)
        strip = struct.unpack_from(BE + '%dH' % W(ir + 4), data, W(hdr + 0x18))
        lst = strip_to_list(strip)
        vcount = vbs[vb_by_rec[W(prim + 8)]]['count']
        if lst and max(lst) >= vcount:
            raise ValueError('index %d >= vertex count %d' % (max(lst), vcount))
        wide = len(lst) > 0xFFFF
        put(ir, 0)
        put(ir + 4, len(lst))
        put(ir + 12, pos * 8 | (2 if wide else 0))
        lists.append(lst)
        pos += len(lst) * (2 if wide else 1)
        for w in (0, 4, 0x14, 0x1c):
            raw(hdr + w)
    ib = b''.join(struct.pack(LE + '%d%s' % (len(x), 'I' if len(x) > 0xFFFF else 'H'), *x) for x in lists)

    # 2. vertex buffers
    vorder = []
    for _part, vb in ent_vb:
        if vb not in vorder:
            vorder.append(vb)
    vorder += [i for i in range(nvb) if i not in vorder]
    vdata = bytearray()
    for i in vorder:
        v = vbs[i]
        n = v['stride'] * v['count']
        if v['off'] + n > len(data):
            raise ValueError('vertex buffer %d outside the data' % i)
        v['new'] = len(ib) + len(vdata)
        v['src'] = swap16(data[v['off']:v['off'] + n])
        vdata += v['src']
        put(v['rec'], 0)
        put(v['rec'] + 4, v['new'])

    # 3. deform streams
    gorder = []
    for _part, vb in ent_vb:
        if body and vbs[vb]['stride'] >= 0x18 and vb not in gorder:
            gorder.append(vb)
    ext, groups = bytearray(), []
    ebase = len(ib) + len(vdata)
    for vb in gorder:
        v = vbs[vb]
        n, st = v['count'], v['stride']
        per, nch = ((n + 1) // 2 * 2, 1) if n <= 512 else (512, (n + 1023) // 1024 * 2)
        tot = per * nch
        s1, s2 = bytearray(), bytearray()
        for i in range(n):
            vv = v['src'][st * i:st * (i + 1)]
            s1 += vv[0:14] + b'\0\0'
            s2 += vv[14:22]
        offs = []
        for blk in (bytes(s1) + bytes(16 * (tot - n)), bytes(s2) + bytes(8 * (tot - n)), bytes(8 * tot)):
            offs.append(ebase + len(ext))
            ext += blk
        groups.append((vb, per, nch, 16 * tot, offs[0], 8 * tot, offs[1], 8 * tot, offs[2]))

    units = []
    for g in gorder:
        units += [w for part, vb in ent_vb if vb == g for w in (part, 0, 0, 2, 0, 0x10, part, 0, 1, 2, 8, 0x10)]
    nunits = len(units) // 6
    pre = [nunits, 0x10 if nunits else 0, len(groups), (0x10 + 0x18 * nunits) if groups else 0] + units
    for g in groups:
        pre += list(g)
    prefix = struct.pack(LE + '%dI' % len(pre), *pre)

    newdata = ib + bytes(vdata) + bytes(ext)
    out = bytearray(c[0:2] + c[3:4] + c[2:3])
    out += struct.pack(LE + 'I', len(prefix)) + prefix
    out += swap32(c[4:s + 4])
    out += bytes(-len(out) % 16)
    out += obj
    if stream is not None:
        out += swap32(c[t:])
        return bytes(out), struct.pack(LE + '3I', 0, len(newdata), 0) + newdata
    tr = list(struct.unpack_from(BE + '5I', c, t))
    tr[3] = len(newdata)
    out += struct.pack(LE + '5I', *tr) + newdata
    return bytes(out)


def is_body(path):
    name = os.path.basename(path).upper()
    return bool(re.search(r'_(XLOW|LOW|MID|HIGH)\.MODEL$', name)) and '_FW_' not in name and '_BW_' not in name


def compare(a, path):
    b = open(path, 'rb').read()
    d = sum(1 for x, y in zip(a, b) if x != y) + abs(len(a) - len(b))
    return 'identical' if d == 0 else '%d bytes differ (length %d / %d)' % (d, len(a), len(b))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--stream', nargs=2, metavar=('IN', 'OUT'), help='console and PC .model.stream')
    ap.add_argument('--expect', nargs='+', metavar='PC', help='real PC file(s) to compare with')
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--deform', dest='body', action='store_true', default=None)
    g.add_argument('--no-deform', dest='body', action='store_false')
    a = ap.parse_args(argv)
    body = is_body(a.src) if a.body is None else a.body
    c = open(a.src, 'rb').read()
    if a.stream:
        m, st = convert(c, body=body, stream=open(a.stream[0], 'rb').read())
        open(a.dst, 'wb').write(m)
        open(a.stream[1], 'wb').write(st)
        res = [m, st]
    else:
        m = convert(c, body=body)
        open(a.dst, 'wb').write(m)
        res = [m]
    print('%s: %d -> %d bytes, deform streams %s' % (a.src, len(c), len(m), 'yes' if body else 'no'))
    for r, p in zip(res, a.expect or []):
        print('  vs %s: %s' % (p, compare(r, p)))


if __name__ == '__main__':
    sys.exit(main())
