#!/usr/bin/env python3
"""combo_sigs.py - interface signature of every shader combo in Split/Second archives.

For each combo (NUL-terminated id "0x%08x" inside an .fxc entry) prints the names
of the shader constants and samplers its vertex and pixel programs declare, read
from the constant tables (CTAB) of the programs that follow the id.  Two combos with
the same signature take the same inputs from the engine, so a missing combo can be
replaced by a present one with the same signature at little risk.

    combo_sigs.py <reader dir> <be|le> <archive> [<archive> ...]
Output: <archive> <entry hash> <combo id> vs:<names,...> ps:<names,...>
"""
import os, re, struct, sys

ID = re.compile(rb"(?<=[\x00-\x20])0x[0-9a-f]{8}(?=\x00)")


def ctab_names(blob, pos, end, e):
    """Constant names of every CTAB in blob[pos:end]."""
    out = []
    i = pos
    while True:
        j = blob.find(b"CTAB", i, end)
        if j < 0:
            return out
        t = j + 4
        try:
            size, creator, version, n, cinfo = struct.unpack_from(e + "5I", blob, t)
            if size != 28 or n > 512:
                i = j + 4
                continue
            names = []
            for k in range(n):
                nm = struct.unpack_from(e + "I", blob, t + cinfo + 20 * k)[0]
                q = blob.find(b"\x00", t + nm, t + nm + 128)
                names.append(blob[t + nm:q].decode("latin-1"))
            out.append(sorted(names))
        except struct.error:
            pass
        i = j + 4


def main():
    sys.path.insert(0, sys.argv[1])
    big = sys.argv[2] == "be"
    mod = __import__("ark_be" if big else "ark_le")
    e = ">" if big else "<"
    for path in sys.argv[3:]:
        arc = mod.Archive(path)
        for ent in arc.entries:
            if ent.usize < 8 or arc.content(ent, 4) not in (b"\x00FXC", b"CXF\x00"):
                continue
            blob = arc.content(ent)
            ms = list(ID.finditer(blob))
            # an id occurs several times on Xbox (combo header and debug paths); keep the first of each
            seen = set()
            heads = []
            for m in ms:
                if m.group() not in seen:
                    seen.add(m.group())
                    heads.append((m.start(), m.group().decode()))
            for k, (pos, cid) in enumerate(heads):
                end = heads[k + 1][0] if k + 1 < len(heads) else len(blob)
                if big:
                    # Xbox 360 shader containers carry a D3DX constant table without the CTAB tag
                    tabs = []
                    for off, size in mod.find_containers(blob[pos:end]):
                        ok, _t, _n, names = mod.check_ctab(blob[pos:end], off)
                        if ok:
                            tabs.append(sorted(names))
                else:
                    tabs = ctab_names(blob, pos, end, e)
                sig = " ".join("t%d:%s" % (n, ",".join(t)) for n, t in enumerate(tabs))
                print("%s %08x %s %s" % (os.path.basename(path), ent.hash, cid, sig))
        sys.stdout.flush()


if __name__ == "__main__":
    main()
