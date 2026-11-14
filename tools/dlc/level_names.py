#!/usr/bin/env python3
"""level_names.py - candidate archive member names for a Split/Second level.

Expands the path format strings that the PC exe uses for level data (taken from
its string table, passed in as a text file, one string per line) with a level
name, subtrack letters, game-mode names and small integers, and prints one
candidate name per line.  Feed the output to an archive lister (`--names`), which
keeps only the names whose hash is in the archive.  No game data is embedded.

    level_names.py <strings.txt> <level> [<level> ...] > names.txt
"""
import itertools, re, sys

SUBTRACKS = list("ABCDEF")
MODES = ["QuickRace", "Race", "TimeTrial", "Elimination", "HelicopterSurvival",
         "HelicopterRevenge", "Nemesis", "NemesisSplitScreen", "NemesisMulti",
         "KingOfTheAir", "NemesisRace", "TimeTrialPlus", "Eliminator", "Survival",
         "Default", "SinglePlayer", "MultiPlayer", "SplitScreen"]
SPEC = re.compile(r"%[-0-9.]*[a-zA-Z]")
DEV = re.compile(r"^[A-Za-z0-9]*(\(noasserts?\))?\|")


def main():
    fmts = [l.rstrip("\n") for l in open(sys.argv[1], encoding="latin-1")]
    fmts = [f for f in fmts if "%s" in f and "/" in f.replace("\\", "/")]
    levels = sys.argv[2:]
    out = set()
    for lvl in levels:
        vals = [lvl, lvl.lower()] + SUBTRACKS + MODES + [lvl + "_" + s for s in SUBTRACKS]
        for f in fmts:
            g = DEV.sub("", f).replace("\\", "/")
            parts = SPEC.split(g)
            n = len(parts) - 1
            if n == 0 or n > 3:
                continue
            pool = vals + [str(i) for i in range(6)]
            for combo in itertools.product(pool, repeat=n):
                if n == 3 and combo[0] not in (lvl, lvl.lower()):
                    continue
                name = "".join(a + b for a, b in zip(parts, list(combo) + [""]))
                out.add(name)
                pre = "Environments/Levels/%s/" % lvl
                if not name.startswith("Environments/"):
                    out.add(pre + name)
                elif name.lower().startswith(pre.lower()):
                    out.add(name[len(pre):])
    sys.stdout.write("\n".join(sorted(out)) + "\n")


if __name__ == "__main__":
    main()
