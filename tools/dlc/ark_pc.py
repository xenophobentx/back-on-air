#!/usr/bin/env python3
"""ark_pc.py - reader and writer for the PC (Steam) Split/Second .ark archives.

Standard library only, Python 3.8+. Reads the user's own archives; writes new archives into an overlay folder.
It never writes into the game folder's own files: the output path must not exist yet or must be given with
--force, and it may not be the input file.

Usage
    ark_pc.py list      <archive> [--names FILE]
    ark_pc.py extract   <archive> <name-or-hash> <outfile>
    ark_pc.py roundtrip <archive> <out.ark>          rewrite unchanged, then compare byte for byte
    ark_pc.py lzsstest  <archive> [N]                re-encode N compressed members, check decode(encode(x)) == x
    ark_pc.py build     <archive> <out.ark> [--put NAME=FILE] [--copy OLD=NEW] [--drop NAME] [--store]

    --put NAME=FILE   add the member NAME, or replace it, with the bytes of FILE (compressed unless --store)
    --copy OLD=NEW    add NEW with the stored bytes of the existing member OLD (no re-compression)
    --drop NAME       leave NAME out
    NAME may be a path (hashed as the engine does) or an 8-digit hex hash (0x1234abcd).

Format (PC, little-endian; reverse engineering of the PC archives, every field checked by `roundtrip`)
    Header 0x18 bytes: u32 version 0x00010000, u32 0x10 + 20*count, u32 count, u32 0x10, u32 0 (named entries),
    u32 = the second field.
    TOC at 0x18: count entries of 20 bytes, sorted by name hash ascending:
        u32 flags (0 stored, 1 LZSS), u32 absolute offset, u32 stored size, u32 size, u32 name hash.
    Data from 0x18 + 20*count: members back to back in their own order (not hash order), no padding between
    them. Level and resident archives are zero-padded to a multiple of 2048 bytes, the sector archives
    (*.static.ark) are not; the writer keeps the source's rule.
    flags 1: u32 size, then an Okumura LZSS stream (N 4096, F 18, threshold 2, ring start 0xFEE, zero fill;
    8 tokens per flag byte, LSB first, 1 = literal; a match is b0 | (b1 & 0xF0) << 4 = ring position,
    (b1 & 15) + 3 = length). The stored size counts the 4-byte prefix.
    Name hash: the lower-cased path, h = c + 31*h (mod 2^32), from 0. Paths are relative to the archive's root
    (resident.ark: "Vehicles/...", level archives: relative to the level folder).

Writer rules: untouched members keep their stored bytes and their place in the data order; a replaced member
keeps its place; new members go after the last one. So rewriting an archive with no change gives the same file.
"""
import argparse
import hashlib
import os
import struct
import sys

HEADER = 0x18
ENTRY = 20
ALIGN = 2048
VERSION = 0x00010000


def name_hash(s):
    h = 0
    for c in s.lower().encode("latin-1", "replace"):
        h = (c + 31 * h) & 0xFFFFFFFF
    return h


def key_hash(k):
    """A path, or an 8-digit hex hash written as 0x........"""
    if k.lower().startswith("0x") and len(k) == 10:
        return int(k, 16)
    return name_hash(k.replace("\\", "/"))


class Member(object):
    __slots__ = ("hash", "flags", "offset", "csize", "usize", "data", "name")

    def __init__(self, h, flags, offset, csize, usize):
        self.hash, self.flags, self.offset, self.csize, self.usize = h, flags, offset, csize, usize
        self.data = None  # stored bytes when the member does not come from the source file
        self.name = None


class Archive(object):
    def __init__(self, path):
        self.path = path
        self.fh = open(path, "rb")
        self.size = os.fstat(self.fh.fileno()).st_size
        head = self.fh.read(HEADER)
        if len(head) < HEADER:
            raise ValueError("%s: too short" % path)
        ver, f4, count, fc, named, f14 = struct.unpack("<6I", head)
        if ver != VERSION or named != 0 or fc != 0x10 or f4 != 0x10 + ENTRY * count or f14 != f4:
            raise ValueError("%s: not a PC archive header this tool knows (%08x %08x %u %08x %u %08x)"
                             % (path, ver, f4, count, fc, named, f14))
        raw = self.fh.read(ENTRY * count)
        self.members = []
        for i in range(count):
            fl, off, cs, us, h = struct.unpack_from("<5I", raw, ENTRY * i)
            if fl not in (0, 1):
                raise ValueError("%s: member %08x has flags %u" % (path, h, fl))
            self.members.append(Member(h, fl, off, cs, us))
        self.by_hash = {m.hash: m for m in self.members}
        if len(self.by_hash) != count:
            raise ValueError("%s: duplicate hashes" % path)

    def close(self):
        self.fh.close()

    def stored(self, m):
        if m.data is not None:
            return m.data
        self.fh.seek(m.offset)
        b = self.fh.read(m.csize)
        if len(b) != m.csize:
            raise ValueError("%s: member %08x runs past the end" % (self.path, m.hash))
        return b

    def content(self, m):
        b = self.stored(m)
        if m.flags == 0:
            return b
        if struct.unpack_from("<I", b)[0] != m.usize:
            raise ValueError("member %08x: size prefix differs from the TOC" % m.hash)
        return lzss_decode(b[4:], m.usize)

    def get(self, key):
        return self.by_hash.get(key_hash(key))


def lzss_decode(src, usize):
    out = bytearray(4096)
    cur, i, n, end = 4096, 0, len(src), 4096 + usize
    while cur < end and i < n:
        fl = src[i]
        i += 1
        for _ in range(8):
            if cur >= end:
                break
            if fl & 1:
                if i >= n:
                    break
                out.append(src[i])
                i += 1
                cur += 1
            else:
                if i + 1 >= n:
                    i = n
                    break
                b0, b1 = src[i], src[i + 1]
                i += 2
                p = b0 | ((b1 & 0xF0) << 4)
                ln = (b1 & 15) + 3
                d = (cur + 0xFEE - p) & 0xFFF or 4096
                j = cur - d
                for k in range(ln):  # byte by byte: an overlapping copy repeats, as the ring does
                    out.append(out[j + k])
                cur += ln
            fl >>= 1
    if cur < end:
        raise ValueError("LZSS stream ends at %u of %u bytes" % (cur - 4096, usize))
    return bytes(out[4096:end])


def lzss_encode(data):
    """Greedy LZSS with hash chains; output decodes with the engine's decoder (distances 1..4078, length 3..18).
    The ring's zero fill is not used as a dictionary, so the stream is valid whatever the fill."""
    n = len(data)
    out = bytearray()
    heads = {}
    prev = [0] * n if n else []
    i = 0
    flag_pos, bit, flags = -1, 8, 0
    MAXD, MAXL, CHAIN = 4078, 18, 64
    while i < n:
        if bit == 8:
            if flag_pos >= 0:
                out[flag_pos] = flags
            flag_pos = len(out)
            out.append(0)
            bit, flags = 0, 0
        best_len, best_d = 0, 0
        if i + 3 <= n:
            key = data[i:i + 3]
            cand = heads.get(key, -1)
            tries = 0
            lim = min(MAXL, n - i)
            while cand >= 0 and i - cand <= MAXD and tries < CHAIN:
                L = 3
                while L < lim and data[cand + L] == data[i + L]:
                    L += 1
                if L > best_len:
                    best_len, best_d = L, i - cand
                    if L == lim:
                        break
                cand = prev[cand]
                tries += 1
        if best_len >= 3:
            p = (i + 0xFEE - best_d) & 0xFFF
            out.append(p & 0xFF)
            out.append(((p >> 4) & 0xF0) | (best_len - 3))
            step = best_len
        else:
            flags |= 1 << bit
            out.append(data[i])
            step = 1
        for k in range(i, i + step):
            if k + 3 <= n:
                kk = data[k:k + 3]
                prev[k] = heads.get(kk, -1)
                heads[kk] = k
        i += step
        bit += 1
    if flag_pos >= 0:
        out[flag_pos] = flags
    return bytes(out)


def pack_member(content, store=False):
    """(flags, stored bytes) for new content; compressed only when it is smaller."""
    if not store and content:
        enc = struct.pack("<I", len(content)) + lzss_encode(content)
        if lzss_decode(enc[4:], len(content)) != content:
            raise AssertionError("LZSS self-check failed")
        if len(enc) < len(content):
            return 1, enc
    return 0, bytes(content)


def write_archive(src, out_path, puts=(), copies=(), drops=(), store=False, force=False):
    """src: Archive. puts: [(name, bytes)], copies: [(old, new)], drops: [name]. Returns the list of changes."""
    if os.path.abspath(out_path) == os.path.abspath(src.path):
        raise ValueError("the output may not be the input archive")
    if os.path.exists(out_path) and not force:
        raise ValueError("%s exists (use --force)" % out_path)
    # The data order of the source; an empty member shares its offset with the next member and goes first.
    order = sorted(src.members, key=lambda m: (m.offset, m.csize > 0))
    by_hash = {m.hash: m for m in order}
    log = []
    for name in drops:
        h = key_hash(name)
        if h in by_hash:
            order.remove(by_hash.pop(h))
            log.append("drop %08x %s" % (h, name))
    for old, new in copies:
        m = by_hash.get(key_hash(old))
        if m is None:
            raise ValueError("copy: %s not in %s" % (old, src.path))
        h = key_hash(new)
        c = Member(h, m.flags, 0, m.csize, m.usize)
        c.data = src.stored(m)
        c.name = new
        if h in by_hash:
            order[order.index(by_hash[h])] = c
            log.append("copy %s -> %s (replaced %08x)" % (old, new, h))
        else:
            order.append(c)
            log.append("copy %s -> %s (new %08x)" % (old, new, h))
        by_hash[h] = c
    for name, content in puts:
        h = key_hash(name)
        fl, b = pack_member(content, store)
        m = Member(h, fl, 0, len(b), len(content))
        m.data = b
        m.name = name
        if h in by_hash:
            order[order.index(by_hash[h])] = m
            log.append("put %s (replaced %08x, %u bytes, %s)" % (name, h, len(content), "lzss" if fl else "stored"))
        else:
            order.append(m)
            log.append("put %s (new %08x, %u bytes, %s)" % (name, h, len(content), "lzss" if fl else "stored"))
        by_hash[h] = m
    count = len(order)
    data_start = HEADER + ENTRY * count
    off = data_start
    placed = {}
    for m in order:
        placed[m.hash] = off
        off += m.csize
    src_end = max([m.offset + m.csize for m in src.members] + [HEADER + ENTRY * len(src.members)])
    padded = src.size != src_end  # the source pads to 2048 (level and resident archives) or not (sector archives)
    total = (off + ALIGN - 1) // ALIGN * ALIGN if padded else off
    if total >= 1 << 32:
        raise ValueError("archive larger than 4 GB")
    tmp = out_path + ".part"
    with open(tmp, "wb") as f:
        f.write(struct.pack("<6I", VERSION, 0x10 + ENTRY * count, count, 0x10, 0, 0x10 + ENTRY * count))
        toc = bytearray()
        for h in sorted(by_hash):
            m = by_hash[h]
            toc += struct.pack("<5I", m.flags, placed[h], m.csize, m.usize, h)
        f.write(toc)
        for m in order:
            f.write(src.stored(m))
        f.write(b"\0" * (total - off))
    os.replace(tmp, out_path)
    return log


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list"); p.add_argument("archive"); p.add_argument("--names")
    p = sub.add_parser("extract"); p.add_argument("archive"); p.add_argument("name"); p.add_argument("out")
    p = sub.add_parser("roundtrip"); p.add_argument("archive"); p.add_argument("out")
    p = sub.add_parser("lzsstest"); p.add_argument("archive"); p.add_argument("n", nargs="?", type=int, default=20)
    p = sub.add_parser("build"); p.add_argument("archive"); p.add_argument("out")
    p.add_argument("--put", action="append", default=[]); p.add_argument("--copy", action="append", default=[])
    p.add_argument("--drop", action="append", default=[]); p.add_argument("--store", action="store_true")
    p.add_argument("--force", action="store_true")
    a = ap.parse_args(argv)
    arc = Archive(a.archive)
    if a.cmd == "list":
        names = {}
        if a.names:
            for line in open(a.names, encoding="latin-1"):
                s = line.strip()
                if s:
                    names[name_hash(s)] = s
        for m in sorted(arc.members, key=lambda m: m.offset):
            print("%08x %d %10u %10u %10u %s" % (m.hash, m.flags, m.offset, m.csize, m.usize, names.get(m.hash, "")))
    elif a.cmd == "extract":
        m = arc.get(a.name)
        if m is None:
            sys.exit("%s: not in the archive" % a.name)
        with open(a.out, "wb") as f:
            f.write(arc.content(m))
    elif a.cmd == "roundtrip":
        write_archive(arc, a.out, force=True)
        same = os.path.getsize(a.out) == arc.size and sha1(a.out) == sha1(a.archive)
        print("%s: %u members, %u bytes -> %s: %s" % (a.archive, len(arc.members), arc.size, a.out,
                                                       "byte-identical" if same else "DIFFERENT"))
        sys.exit(0 if same else 1)
    elif a.cmd == "lzsstest":
        done = 0
        for m in sorted(arc.members, key=lambda m: m.csize):
            if m.flags != 1 or m.usize < 256:
                continue
            c = arc.content(m)
            enc = lzss_encode(c)
            ok = lzss_decode(enc, len(c)) == c
            same = enc == arc.stored(m)[4:]
            print("%08x %8u -> stored %8u, ours %8u, round trip %s, same stream %s"
                  % (m.hash, len(c), m.csize - 4, len(enc), "ok" if ok else "FAIL", same))
            if not ok:
                sys.exit(1)
            done += 1
            if done >= a.n:
                break
    elif a.cmd == "build":
        puts = []
        for s in a.put:
            name, path = s.split("=", 1)
            with open(path, "rb") as f:
                puts.append((name, f.read()))
        copies = [tuple(s.split("=", 1)) for s in a.copy]
        for line in write_archive(arc, a.out, puts, copies, a.drop, a.store, a.force):
            print(line)
    arc.close()


if __name__ == "__main__":
    main()
