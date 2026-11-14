#!/usr/bin/env python3
"""overlay_car_gate1.py - builds the overlay of DLC gate 1 (docs/dlc-overlay.md) from the user's own install.

    overlay_car_gate1.py <game folder> <overlay folder> [--base Supercar_08]

Reads <game>/data/resident.ark (never writes into the game folder) and writes <overlay>/data/resident.ark with two
members changed:
* Vehicles/VehicleDatabase.xml: the row Placeholder_01 (an empty slot of the base data) takes every file and stat
  attribute of the base car --base, Locked="false", is a player car (its Player="false" is removed) and gets the next
  free CarSelectOrder.
* UI/Logic/Season.xml: Placeholder_01 is added to <InitialCars>. The car select does not list the database rows: it
  lists the unlock manager's cars (0x47355B-0x473563), which start from InitialCars and add the credit, elite and
  bonus unlocks on every rebuild (0xA05FE0, 0xA05560). The database's Locked flag is read only on the debug
  all-cars path (0x4735DA). InitialCars is re-applied at every rebuild, so an existing save gets the car too.
Every other member keeps its stored bytes.
Then point the patch at it: overlay_dir = <overlay folder> in splitsecond.conf.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_pc  # noqa: E402

VDB = "Vehicles/VehicleDatabase.xml"
SEASON = "UI/Logic/Season.xml"
SLOT = "Placeholder_01"


def find_ci(folder, name):
    """A child of folder by case-insensitive name (Linux installs use lower-case names)."""
    for n in os.listdir(folder):
        if n.lower() == name.lower():
            return os.path.join(folder, n)
    raise SystemExit("%s: no %s" % (folder, name))


def attrs(row):
    return re.findall(r'(\w+)="([^"]*)"', row)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("out")
    ap.add_argument("--base", default="Supercar_08", help="the base car whose files the slot uses")
    a = ap.parse_args()
    src = find_ci(find_ci(a.game, "data"), "resident.ark")
    out = os.path.join(a.out, "data", "resident.ark")
    if os.path.realpath(out).startswith(os.path.realpath(a.game) + os.sep):
        raise SystemExit("the overlay may not be inside the game folder's data (choose a new folder)")
    arc = ark_pc.Archive(src)
    m = arc.get(VDB)
    if m is None:
        raise SystemExit("%s: no %s" % (src, VDB))
    text = arc.content(m).decode("latin-1")
    rows = re.findall(r'<Car [^>]*/>', text)
    base = next((r for r in rows if 'ID="%s"' % a.base in r), None)
    slot = next((r for r in rows if 'ID="%s"' % SLOT in r), None)
    if not base or not slot:
        raise SystemExit("%s: no row for %s or %s" % (VDB, a.base, SLOT))
    order = max(int(v) for r in rows for k, v in attrs(r) if k == "CarSelectOrder") + 1
    kv = [(k, v) for k, v in attrs(base) if k not in ("ID", "CarSelectOrder", "Locked", "Player")]
    new = '<Car ID="%s" %s Locked="false" CarSelectOrder="%d"/>' % (SLOT, " ".join('%s="%s"' % p for p in kv), order)
    text2 = text.replace(slot, new, 1)
    sm = arc.get(SEASON)
    if sm is None:
        raise SystemExit("%s: no %s" % (src, SEASON))
    season = arc.content(sm).decode("latin-1")
    init = re.search(r"<InitialCars>.*?</InitialCars>", season, re.S)
    if not init:
        raise SystemExit("%s: no <InitialCars> list" % SEASON)
    if 'Name="%s"' % SLOT in init.group(0):
        season2 = season
    else:
        nl = "\r\n" if "\r\n" in init.group(0) else "\n"
        close = init.start() + init.group(0).rindex("</InitialCars>")
        season2 = season[:close] + '  <Car Name="%s"/>%s  ' % (SLOT, nl) + season[close:]
    os.makedirs(os.path.dirname(out), exist_ok=True)
    puts = [(VDB, text2.encode("latin-1")), (SEASON, season2.encode("latin-1"))]
    for line in ark_pc.write_archive(arc, out, puts=puts, force=True):
        print(line)
    print("old row: " + slot)
    print("new row: " + new)
    print("initial cars: " + " ".join(re.findall(r'<Car Name="([^"]+)"', re.search(r"<InitialCars>.*?</InitialCars>", season2, re.S).group(0))))
    print("wrote %s (%u bytes); set overlay_dir = %s" % (out, os.path.getsize(out), os.path.abspath(a.out)))


if __name__ == "__main__":
    main()
