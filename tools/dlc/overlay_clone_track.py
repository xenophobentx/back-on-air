#!/usr/bin/env python3
"""overlay_clone_track.py - builds the clone-track test overlay (docs/dlc-tracks.md section 7, docs/dlc-overlay.md)
from the user's own install: a PC level written as a new level under a new name, track id 15, listed last in one
quick-play mode and unlocked.

    overlay_clone_track.py <game folder> <overlay folder> [--src airport_test_03] [--dst airport_clone]
                           [--mode QuickRace] [--event 1:4] [--lightrig midday] [--names FILE]

Default: the race level airport_test_03 (the smallest race level), subtrack A, as airport_clone, the 12th tile of
Quick Play > Quick Race (the stock list has 11, so the 12th tile is empty).

Reads the game folder only. Writes into <overlay>:
    data/environments/levels/<dst>.ark                 the level archive
    data/environments/levels/<dst>/sectors/<dst>.static.ark, <dst>.stream.ark   the sector archives
    data/resident.ark                                  per-level resident files under the new name, directory.txt
                                                       (TRACKS + <dst>_SUBTRACK: A), UI/Logic/TrackFiltration.xml
                                                       (<dst> A last in <mode>), UI/Logic/Season.xml (one career
                                                       event moved to <dst> A, see below)
    data/seclusion.ark                                 the seclusion zones under the new name
    dlc-tracks.txt                                     "15 <dst> A <src>_A" for dlc_tracks = true

Member names: the archives hold only name hashes. A member the engine finds through a path built from the level
name (its format strings: Physics/%s.hkColl, Subtracks/%s/%s.entities, Resident|Environments/Levels/%s/%s_po.textures,
...) gets a copy under the new name; every member also stays under its old hash, so the names stored inside the data
(which still say <src>) keep resolving. Candidate names are generated below; --names adds more (one path per line,
with the old level name in it). The script lists which candidates hit. Sector archives are copied whole (their members
are found by names stored in the level data).

Quick-play lock: the per-mode quick-play list (builder 0x77ACB0, read by 0x4851C0) starts every track locked and
unlocks it when a career event of Season.xml (12 episodes, 0x775240) has the same Track (+0x48) and SubTrack (+0x90)
and that event is either a Qualifier (+0xE4 == 1) of Episode 1, which counts without any save data, or unlocked
(+0x138 == 0) and finished (+0xF0 FinishPosition != -1) in the save. 0xD6FC54 is the debug unlock-all. So --event
EPISODE:N moves the N-th event of that episode (it must be an Episode 1 Qualifier to unlock without the save) to
<dst> A (--lightrig sets its light rig, since the clone's rigs are the source level's). Default 1:4, Episode 1
"EXTRA EVENT 1" (Elimination, Downtown A, sunset_02), now Elimination on airport_clone A, midday; Downtown A stays
unlocked through Episode 1 event 2.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ark_pc  # noqa: E402

SUBS = list("ABCDEF")
EXTS = ["", ".model", ".model.stream", ".fxc", ".shaders", ".shaderinst", ".textures", ".low.textures",
        ".texture.crcs", ".tex.crcs", ".streamtex", ".xml", ".params", ".data", ".coronas"]
MODES = ["QuickRace", "Race", "TimeTrial", "Elimination", "HelicopterSurvival", "HelicopterRevenge", "Nemesis",
         "NemesisSplitScreen", "NemesisMulti", "Survival", "Default"]
AUDIO = ["", "_Swooshes", "_Ambience", "_Ambient", "_Reverb", "_Music", "_Triggers", "_Emitters", "_Zones"]

# Paths relative to the level folder (the level archive), with {L} = level name, {S} = subtrack, {d} = digit.
LEVEL = ["Physics/{L}.hkColl", "Physics/{L}.hkHBXs", "Physics/{L}.hkRBXs", "Subtracks/{S}/{L}.entities",
         "Subtracks/{S}/{L}.triggers", "Subtracks/{S}/{L}.hkPPs", "Subtracks/{S}/{L}.hkRBs",
         "Subtracks/{S}/{L}_{S}.seclusion", "Subtracks/{S}/{L}.powerplays", "Backdrop/{L}_backdrop{d}{E}",
         "BasicCoronas/{L}_coronas{E}", "Sectors/{L}{E}", "{L}{E}", "{L}_po{E}", "{L}_collision_tl.xml",
         "Overlays/{L}{E}", "Skydome/{L}{E}", "LensFlare/{L}{E}"]
# Full paths in resident.ark.
RESIDENT = ["Environments/Levels/{L}/{L}_po.textures", "Environments/Levels/{L}/Cameras/CameraGroups.xml",
            "Environments/Levels/{L}/Cameras/Cameras.xml", "Environments/Levels/{L}/Cameras/CameraSplines.xml",
            "Environments/Levels/{L}/Cameras/CameraZones.xml", "Environments/Levels/{L}/Cameras/PowerplayCameras.xml",
            "Environments/Levels/{L}/Cameras/PowerplayCameraSubjects.xml",
            "Environments/Levels/{L}/Cameras/TakedownSequences/TakeDownSequences.xml",
            "Environments/Levels/{L}/Cameras/GameModes/{M}/Cameras.xml",
            "Environments/Levels/{L}/Cameras/GameModes/{M}/{S}/Cameras.xml",
            "Environments/Levels/{L}/TyreTracks/TyreTrack.params", "Environments/Levels/{L}/TyreTracks/TyreTracks",
            "Environments/Levels/{L}/HelicopterMissileTargets.xml", "Environments/Levels/{L}/Debris/backed.hk",
            "Environments/Levels/{L}/Debris/nonbacked.hk", "Environments/Levels/{L}/Subtracks/{S}/PowerplayMaterials.params",
            "Environments/Levels/{L}/Subtracks/{S}/Track.nis", "Environments/Levels/{L}/Subtracks/{S}/Track.lip",
            "Environments/Levels/{L}/Params/PPD/{M}.params", "Environments/Levels/{L}/{L}_collision_tl.xml",
            "Environments/Levels/{L}/Subtracks/{S}/{L}.powerplays",
            "GameLogic/Tracks/{L}/Params/RaceSetupOverlay.params", "GameLogic/Tracks/{L}/Params/RaceSetupOverlay_{S}.params",
            "GameLogic/Tracks/{L}/Params/RaceSetupOverlay_{S}{d}.params", "Audio/Locales/{L}/{L}_Audio{A}.xml"]
SECLUSION = ["Environments/Levels/{L}/Subtracks/{S}/{L}_{S}.seclusion"]


def expand(fmts, lvl):
    out = set()
    for f in fmts:
        for s in (SUBS if "{S}" in f else [""]):
            for d in (range(6) if "{d}" in f else [""]):
                for e in (EXTS if "{E}" in f else [""]):
                    for m in (MODES if "{M}" in f else [""]):
                        for a in (AUDIO if "{A}" in f else [""]):
                            out.add(f.format(L=lvl, S=s, d=d, E=e, M=m, A=a))
    return out


def find_ci(folder, *parts):
    p = folder
    for name in parts:
        hit = [n for n in os.listdir(p) if n.lower() == name.lower()]
        if not hit:
            raise SystemExit("%s: no %s" % (p, name))
        p = os.path.join(p, hit[0])
    return p


def renames(arc, cands, src, dst, what):
    """(old, new) pairs for the candidates that are members of arc."""
    pairs = []
    for c in sorted(cands):
        if arc.get(c) is not None:
            new = re.sub(re.escape(src), dst, c, flags=re.I)
            if arc.get(new) is None:
                pairs.append((c, new))
    print("%s: %d level-named members found: %s" % (what, len(pairs), ", ".join(p[0] for p in pairs) or "none"))
    return pairs


def copy_file(src, dst):
    with open(src, "rb") as f, open(dst + ".part", "wb") as g:
        for b in iter(lambda: f.read(1 << 20), b""):
            g.write(b)
    os.replace(dst + ".part", dst)


def edit_directory(text, src, dst):
    lines = text.split("\r\n")
    i = lines.index("\t:TRACKS")
    j = i + 2
    while j < len(lines) and lines[j].startswith("\t") and not lines[j].startswith("\t:"):
        j += 1
    tracks = lines[i + 2:j]
    if "\t" + dst in tracks:
        raise SystemExit("directory.txt already lists %s" % dst)
    tracks = sorted(tracks + ["\t" + dst], key=lambda s: s.lower())
    lines[i + 2:j] = tracks
    block = ["-Environments/Levels/%s/Subtracks/*" % dst, "\t:%s_SUBTRACK" % dst, "\t:1", "\tA"]
    k = lines.index("-Environments/Levels/%s/Subtracks/*" % src)
    lines[k:k] = block
    return "\r\n".join(lines)


def edit_filtration(text, mode, dst):
    m = re.search(r'<GameMode Name="%s"[^>]*>.*?</Tracks>' % re.escape(mode), text, re.S)
    if not m:
        raise SystemExit("TrackFiltration.xml: no %s mode" % mode)
    body = m.group(0)
    t = list(re.finditer(r'([ \t]*)<Track Name="[^"]+" SubTrack="[^"]+"/>[^\r\n]*\r?\n', body))
    if not t:
        raise SystemExit("TrackFiltration.xml: %s has no tracks" % mode)
    add = '%s<Track Name="%s" SubTrack="A"/>\r\n' % (t[0].group(1), dst)
    body2 = body[:t[-1].end()] + add + body[t[-1].end():]
    print("TrackFiltration.xml: %s A is tile %d of %s" % (dst, len(t) + 1, mode))
    return text[:m.start()] + body2 + text[m.end():]


def edit_season(text, event, dst, lightrig):
    ep, n = (int(x) for x in event.split(":"))
    m = re.search(r'<Episode Number="%d">.*?</Episode>' % ep, text, re.S)
    if not m:
        raise SystemExit("Season.xml: no Episode %d" % ep)
    ev = list(re.finditer(r'<Event [^>]*>', m.group(0)))
    if not 1 <= n <= len(ev):
        raise SystemExit("Season.xml: Episode %d has %d events" % (ep, len(ev)))
    e = ev[n - 1]
    if ep != 1 or 'Type="Qualifier"' not in e.group(0):
        print("Season.xml: warning: not an Episode 1 Qualifier, the unlock then needs it finished in the save")
    new = re.sub(r'Track="[^"]*" SubTrack="[^"]*"', 'Track="%s" SubTrack="A"' % dst, e.group(0))
    if lightrig:
        new = re.sub(r'LightRigName="[^"]*"', 'LightRigName="%s"' % lightrig, new)
    print("Season.xml: Episode %d event %d: %s -> %s" % (ep, n, e.group(0), new))
    p = m.start() + e.start()
    return text[:p] + new + text[p + len(e.group(0)):]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("out")
    ap.add_argument("--src", default="airport_test_03")
    ap.add_argument("--dst", default="airport_clone")
    ap.add_argument("--mode", default="QuickRace", help="quick-play mode that lists the clone (Nemesis = Survival)")
    ap.add_argument("--event", default="1:4", help="EPISODE:N of Season.xml moved to the clone (unlocks it)")
    ap.add_argument("--lightrig", default="midday", help="light rig for that event ('' keeps it)")
    ap.add_argument("--id", type=int, default=15)
    ap.add_argument("--names", help="more candidate paths, one per line")
    a = ap.parse_args()
    if len(a.dst) != len(a.dst.encode("ascii")) or not re.match(r"^[A-Za-z0-9_]+$", a.dst):
        raise SystemExit("--dst: letters, digits and _ only")
    data = find_ci(a.game, "data")
    outdata = os.path.join(a.out, "data")
    if os.path.realpath(a.out).startswith(os.path.realpath(data)):
        raise SystemExit("the overlay may not be inside the game's data folder")
    extra = set()
    if a.names:
        extra = {l.strip().replace("\\", "/") for l in open(a.names, encoding="latin-1") if a.src.lower() in l.lower()}
    lvl_dir = os.path.join(outdata, "environments", "levels")
    sec_dir = os.path.join(lvl_dir, a.dst, "sectors")
    os.makedirs(sec_dir, exist_ok=True)

    # 1. level archive
    arc = ark_pc.Archive(find_ci(data, "environments", "levels", a.src + ".ark"))
    pairs = renames(arc, expand(LEVEL, a.src) | extra, a.src, a.dst, a.src + ".ark")
    ark_pc.write_archive(arc, os.path.join(lvl_dir, a.dst + ".ark"), copies=pairs, force=True)
    arc.close()

    # 2. sector archives: copied whole under the new file names
    for kind in ("static", "stream"):
        try:
            s = find_ci(data, "environments", "levels", a.src, "sectors", "%s.%s.ark" % (a.src, kind))
        except SystemExit:
            print("no %s.%s.ark" % (a.src, kind))
            continue
        copy_file(s, os.path.join(sec_dir, "%s.%s.ark" % (a.dst, kind)))
        print("copied %s.%s.ark as %s.%s.ark" % (a.src, kind, a.dst, kind))

    # 3. resident.ark: level files under the new name, directory.txt, TrackFiltration.xml, Season.xml
    arc = ark_pc.Archive(find_ci(data, "resident.ark"))
    pairs = renames(arc, expand(RESIDENT, a.src) | extra, a.src, a.dst, "resident.ark")
    d = arc.content(arc.get("directory.txt")).decode("latin-1")
    f = arc.content(arc.get("UI/Logic/TrackFiltration.xml")).decode("latin-1")
    puts = [("directory.txt", edit_directory(d, a.src, a.dst).encode("latin-1")),
            ("UI/Logic/TrackFiltration.xml", edit_filtration(f, a.mode, a.dst).encode("latin-1")),
            ("UI/Logic/Season.xml", edit_season(arc.content(arc.get("UI/Logic/Season.xml")).decode("latin-1"),
                                                a.event, a.dst, a.lightrig).encode("latin-1"))]
    for line in ark_pc.write_archive(arc, os.path.join(outdata, "resident.ark"), puts=puts, copies=pairs, force=True):
        print("resident.ark: " + line)
    arc.close()

    # 4. seclusion.ark
    arc = ark_pc.Archive(find_ci(data, "seclusion.ark"))
    pairs = renames(arc, expand(SECLUSION, a.src) | extra, a.src, a.dst, "seclusion.ark")
    ark_pc.write_archive(arc, os.path.join(outdata, "seclusion.ark"), copies=pairs, force=True)
    arc.close()

    with open(os.path.join(a.out, "dlc-tracks.txt"), "w") as g:
        g.write("# id level subtrack movie_from (read by the patch when dlc_tracks = true)\n")
        g.write("%d %s A %s_A\n" % (a.id, a.dst, a.src))
    print("wrote %s; set overlay_dir = %s and dlc_tracks = true" % (a.out, os.path.abspath(a.out)))


if __name__ == "__main__":
    main()
