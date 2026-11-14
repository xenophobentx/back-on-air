#!/usr/bin/env python3
"""combo_ids.py - list the shader combo ids of every .fxc entry in Split/Second archives.

An .fxc entry holds one or more shader combos; each is named by a NUL-terminated
id string "0x%08x" (same id on Xbox 360 and PC).  Prints one line per combo:
    <archive> <entry hash> <entry name or -> <combo id>
Needs the archive reader module (ark_be.py for Xbox archives, ark_le.py for PC)
in the directory given as the first argument.

    [COMBO_NAMES=1] combo_ids.py <reader dir> <be|le> <archive> [<archive> ...]
"""
import os, re, sys

ID = re.compile(rb"(?<=[\x00-\x20])0x[0-9a-f]{8}(?=\x00)")


def main():
    sys.path.insert(0, sys.argv[1])
    mod = __import__("ark_be" if sys.argv[2] == "be" else "ark_le")
    for path in sys.argv[3:]:
        arc = mod.Archive(path)
        names = {}
        if os.environ.get("COMBO_NAMES") == "1":
            names = mod.harvest_names(arc)
        for e in arc.entries:
            if e.usize < 8:
                continue
            head = arc.content(e, 4)
            if head not in (b"\x00FXC", b"CXF\x00"):
                continue
            blob = arc.content(e)
            ids = sorted(set(m.group().decode() for m in ID.finditer(blob)))
            nm = names.get(e.hash, "-")
            for i in ids:
                print("%s %08x %s %s" % (os.path.basename(path), e.hash, nm, i))
        sys.stdout.flush()


if __name__ == "__main__":
    main()
