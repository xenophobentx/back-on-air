#!/usr/bin/env python3
"""merge.py OUT_DIR IN_DIR [IN_DIR ...] : merges pipeline_warmup records (pipeline-warmup/<level>.bin, src/pipewarm.cpp)
per level into OUT_DIR as version 3 files: every distinct combination once, in first-seen order, then every derived
entry (a variant of ours -> the game shader it is made of) once. Version 1 shader
records (the game's byte code) are dropped, so the output never holds game data. boot.bin and frontend.bin are skipped
(the replay runs at a level load only). merge.py --list DIR prints levels, combinations, shaders and sizes.
merge.py --check DIR exits 1 unless every file is a clean version 3 record (make dist runs it on the shipped set)."""
import os, struct, sys

MAGIC, TAG_COMBO, TAG_SHADER, TAG_DERIVED = 0x57505353, 0x4F424D43, 0x52444853, 0x56495244


def fnv(b, h=0xcbf29ce484222325):
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def read(path):
    """([(hash, payload)] of the file's combinations, {variant: payload} of its derived entries); checks every
    record's hash like ReadLevel."""
    data = open(path, 'rb').read()
    magic, ver = struct.unpack_from('<II', data, 0)
    if magic != MAGIC or ver not in (1, 2, 3):
        raise SystemExit(f'{path}: not a record')
    out, der, at = [], {}, 8
    while at + 16 <= len(data):
        tag, n, h = struct.unpack_from('<IIQ', data, at)
        p = data[at + 16:at + 16 + n]
        if len(p) != n or fnv(p) != h:
            print(f'{path}: bad record at {at}; rest ignored', file=sys.stderr)
            break
        if tag == TAG_COMBO:
            out.append((h, p))
        elif tag == TAG_DERIVED and n == 24:
            der.setdefault(struct.unpack_from('<Q', p, 0)[0], p)
        elif not (tag == TAG_SHADER and ver == 1):
            print(f'{path}: unknown tag at {at}; rest ignored', file=sys.stderr)
            break
        at += 16 + n
    return out, der


def shaders(combos):
    return {struct.unpack_from('<QQ', p, 0) for _, p in combos}


def check(d):
    """Exit 1 unless every <level>.bin in d is a version 3 record of combinations and derived entries only, every
    record intact (no game byte code: version 1 shader records are refused)."""
    bad = 0
    for f in sorted(os.listdir(d)):
        if not f.endswith('.bin'):
            continue
        data = open(os.path.join(d, f), 'rb').read()
        magic, ver = struct.unpack_from('<II', data, 0) if len(data) >= 8 else (0, 0)
        at, ok = 8, magic == MAGIC and ver == 3
        while ok and at + 16 <= len(data):
            tag, n, h = struct.unpack_from('<IIQ', data, at)
            p = data[at + 16:at + 16 + n]
            ok = len(p) == n and fnv(p) == h and tag in (TAG_COMBO, TAG_DERIVED)
            at += 16 + n
        ok = ok and at == len(data)
        print(f'{f}: {"ok" if ok else "REFUSED (not a clean version 3 record)"}')
        bad += not ok
    sys.exit(1 if bad else 0)


def main(a):
    if a[0] == '--check':
        check(a[1])
    if a[0] == '--list':
        for f in sorted(os.listdir(a[1])):
            if f.endswith('.bin'):
                c, der = read(os.path.join(a[1], f))
                s = shaders(c)
                print(f'{f[:-4]:16s} {len(c):6d} combinations {len({x for p in s for x in p}):5d} shaders {len(der):4d} derived '
                      f'{os.path.getsize(os.path.join(a[1], f)):8d} bytes')
        return
    out, ins = a[0], a[1:]
    levels = {}
    for d in ins:
        for f in sorted(os.listdir(d)):
            if f.endswith('.bin') and f not in ('boot.bin', 'frontend.bin'):
                levels.setdefault(f[:-4], []).append(os.path.join(d, f))
    os.makedirs(out, exist_ok=True)
    for lv, files in sorted(levels.items()):
        seen, merged, per, ders = set(), [], [], {}
        for f in files:
            c, der = read(f)
            for k, v in der.items():
                ders.setdefault(k, v)
            new = [x for x in c if x[0] not in seen]
            seen.update(h for h, _ in new)
            merged += new
            per.append(f'{os.path.basename(os.path.dirname(f))}:{len(c)}(+{len(new)})')
        with open(os.path.join(out, lv + '.bin'), 'wb') as o:
            o.write(struct.pack('<II', MAGIC, 3))
            for h, p in merged:
                o.write(struct.pack('<IIQ', TAG_COMBO, len(p), h) + p)
            for p in ders.values():
                o.write(struct.pack('<IIQ', TAG_DERIVED, len(p), fnv(p)) + p)
        print(f'{lv:16s} {len(merged):6d} combinations {len(ders):4d} derived  ' + ' '.join(per))


main(sys.argv[1:])
