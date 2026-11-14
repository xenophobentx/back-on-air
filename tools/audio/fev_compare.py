#!/usr/bin/env python3
"""fev_compare.py - compare the audio mix data of two Split/Second builds (PC vs Xbox 360).

Standard library only for `fev` and `mixer`; `fsb` also needs numpy and ffmpeg on PATH.
No game data lives in this repository: point the tool at your own extracted copies.

Subcommands
    fev   <pc_dirs> <xb_dirs>            FMOD Designer event files (.fev), paired by file name
                                         (case-insensitive; ARK hash names or loose bank names;
                                         several directories separated by commas)
    mixer <pc_DynamicMixer.xml> <xb_DynamicMixer.xml>
                                         the game's own mixer data (Audio/XML/DynamicMixer.xml,
                                         RESIDENT.ARK entry 912e2ae4.xml on both platforms)
    fsb   <pc_dir> <xb_dir>              per-sample RMS / peak of matching FSB4 banks
                                         (PC MPEG Layer II vs Xbox XMA, decoded with ffmpeg)
    table <pc_dirs> <xb_dirs> [--levels fsb.json] [--header out.h]
                                         per-event corrections for the patch's console_mix
    dump  <file.fev>                     categories and events of one file

Where the data comes from
    PC: data/resident.ark of the Steam build holds 63 .fev and 55 .fsb (little-endian ARK, same
        layout as the Xbox one with the byte order swapped); data/audio/release holds the loose
        music / menu / global / debug banks.
    Xbox: game/RESIDENT.ARK (128 .fev, 112 .fsb: each bank appears under two hashes) and
        game/DEFERRED/AUDIO/SUBMISSION (loose banks). The level ARKs hold no audio banks.
    Both platforms write the event files in the same little-endian FEV1 layout (FMOD Designer
    4.28, version word 0x003D0000); only the sample banks differ (MP2 vs XMA).

FEV1 layout used here (established from the files, not from a specification)
    header: "FEV1", u32 version, two u32 sizes, then length-prefixed strings (bank / project name)
    category tree, depth first, starting at the "master" record:
        u32 name_len (incl. NUL), name, f32 volume (linear), f32 pitch, u32, u32, u32 child_count,
        children...
    event header (simple and complex events):
        u32 type (8 = simple, 0x10 = complex), u32 name_len, name, 16-byte GUID,
        f32 volume (linear), f32 pitch, f32 pitch randomisation, f32 volume randomisation,
        u32 priority, u32 max playbacks, u32 (10000 in every file), u32 mode (0x00180008 = 3D),
        f32 3D min distance, f32 3D max distance, ... (cone, doppler, pan, user properties, layers)
        and, at the end of the block, u32 1, u32 len, category name.
    Events are located by validating that header shape at every candidate offset; the category of
    an event is the last category name string inside its block.
    The last event's block runs to the end of the file, so reverb-definition differences show up
    under it; index bytes that shift by one when a bank gains a sound definition also show up as
    one-byte block differences (they are not level changes).
    Category volumes: every category record is 1.0 on both platforms. The levels the player hears
    are set by the game itself (DynamicMixer.xml stacks and presets, in dB) through the categories'
    FMOD channel groups.
"""
import argparse
import difflib
import glob
import math
import json
import os
import re
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET


def db(lin):
    return -999.0 if lin <= 0 else 20.0 * math.log10(lin)


def lin(dbv):
    return 10.0 ** (dbv / 20.0)


# ----------------------------------------------------------------------------------------- FEV

class Fev:
    def __init__(self, path):
        self.path = path
        self.d = open(path, "rb").read()
        if self.d[:4] != b"FEV1":
            raise ValueError("not an FEV1 file: %s" % path)
        self.version = struct.unpack_from("<I", self.d, 4)[0]
        n = struct.unpack_from("<I", self.d, 16)[0]
        self.project = self.d[20:20 + n - 1].decode("latin1")  # the FMOD project (EventProject::getInfo name)
        self.categories = []      # (path, volume, pitch, raw extra words)
        self.events = []          # dicts, file order
        self._parse_categories()
        self._find_events()
        self.sd_start, self.sounddefs = self._parse_sounddefs()
        self._event_sounddefs()

    # category tree ------------------------------------------------------------------------
    def _parse_categories(self):
        d = self.d
        i = d.find(b"\x07\x00\x00\x00master\x00")
        if i < 0:
            return

        def rec(p, parent):
            n = struct.unpack_from("<I", d, p)[0]
            name = d[p + 4:p + 4 + n - 1].decode("latin1")
            p += 4 + n
            vol, pitch, a, b = struct.unpack_from("<ffII", d, p)
            p += 16
            nc = struct.unpack_from("<I", d, p)[0]
            p += 4
            path = parent + "/" + name if parent else name
            self.categories.append((path, vol, pitch, (a, b)))
            for _ in range(nc):
                p = rec(p, path)
            return p

        self.cat_end = rec(i, "")
        self.cat_names = {c[0].split("/")[-1] for c in self.categories}

    # events -------------------------------------------------------------------------------
    def _event_at(self, p):
        d = self.d
        if p + 8 > len(d):
            return None
        t, n = struct.unpack_from("<II", d, p)
        if t not in (8, 0x10) or not 2 <= n <= 128:
            return None
        name = d[p + 8:p + 8 + n]
        if len(name) != n or name[-1] != 0 or not all(32 <= c < 127 for c in name[:-1]):
            return None
        q = p + 8 + n
        if q + 16 + 40 > len(d):
            return None
        guid = d[q:q + 16]
        if guid.count(0) > 8:
            return None
        q += 16
        vol, pitch, prand, vrand, prio, maxpb, unk, mode, mind, maxd = struct.unpack_from("<4f4I2f", d, q)
        if not (0.0 <= vol <= 16.0 and -100 < pitch < 100 and prio <= 256 and 0 < maxpb <= 4096
                and 0.0 < mind <= maxd < 1e7):
            return None
        return dict(off=p, type=t, name=name[:-1].decode(), guid=guid.hex(), volume=vol, pitch=pitch,
                    pitch_rand=prand, volume_rand=vrand, priority=prio, max_playbacks=maxpb, unk=unk,
                    mode=mode, min_dist=mind, max_dist=maxd, hdr_end=q + 40)

    def _find_events(self):
        d = self.d
        start = getattr(self, "cat_end", 0)
        p = start
        while p < len(d) - 64:
            e = self._event_at(p)
            if e:
                self.events.append(e)
                p = e["hdr_end"]
            else:
                p += 1
        for k, e in enumerate(self.events):
            end = self.events[k + 1]["off"] if k + 1 < len(self.events) else len(d)
            e["end"] = end
            e["category"] = self._last_category(e["hdr_end"], end)

    # sound definitions: the table at the end of the file --------------------------------------
    #   u32 count, then per definition: u32 name_len, name ("/group/def"), u32 property index, u32 entries,
    #   entries: u32 type, u32 weight; type 0 (wave): u32 len, file name, u32 len, bank name, u32 index in the
    #   bank, u32 length in ms; type 1 (oscillator): u32 shape, f32 frequency; type 2 (don't play): nothing more.
    def _parse_sounddefs(self):
        d = self.d
        for m in re.finditer(rb"(....)(....)/", d):
            cnt, n = struct.unpack("<II", m.group(1) + m.group(2))
            if not (0 < cnt < 5000 and 2 < n < 200 and d[m.start() + 8 + n - 1:m.start() + 8 + n] == b"\0"):
                continue
            p, out = m.start() + 4, []
            try:
                for _ in range(cnt):
                    n = struct.unpack_from("<I", d, p)[0]
                    name = d[p + 4:p + 4 + n - 1].decode("latin1")
                    p += 4 + n
                    _prop, nw = struct.unpack_from("<II", d, p)
                    p += 8
                    waves = []
                    for _w in range(nw):
                        t, _wt = struct.unpack_from("<II", d, p)
                        p += 8
                        if t == 0:
                            n1 = struct.unpack_from("<I", d, p)[0]
                            fn = d[p + 4:p + 4 + n1 - 1].decode("latin1")
                            p += 4 + n1
                            n2 = struct.unpack_from("<I", d, p)[0]
                            bank = d[p + 4:p + 4 + n2 - 1].decode("latin1")
                            p += 4 + n2
                            idx, _ms = struct.unpack_from("<II", d, p)
                            p += 8
                            waves.append((fn, bank, idx))
                        elif t == 1:
                            p += 8
                            waves.append(("<oscillator>", None, None))
                        elif t == 2:
                            pass
                        else:
                            raise ValueError("entry type %d" % t)
                    out.append((name, waves))
                return m.start(), out
            except (ValueError, struct.error, UnicodeDecodeError):
                continue
        return None, []

    # which sound definitions an event plays: every sound instance (on a complex event's layers, or the one sound
    # of a simple event) is a record u16 definition index, f32 start, f32 length (both 0..1), u32 0, u32 0|1,
    # u32 -1, ...; simple events store the index as a u32, whose upper half is 0, so the same pattern finds it.
    def _event_sounddefs(self):
        nsd = len(self.sounddefs)
        rec = re.compile(rb"(?=(..)(....)(....)\x00\x00\x00\x00[\x00\x01]\x00\x00\x00\xff\xff\xff\xff)", re.S)
        for e in self.events:
            end = e["end"]
            if self.sd_start is not None and e["off"] < self.sd_start < end:
                end = self.sd_start
            blk = self.d[e["hdr_end"]:end]
            refs = set()
            for m in rec.finditer(blk):
                i = struct.unpack("<H", m.group(1))[0]
                a, b = struct.unpack("<ff", m.group(2) + m.group(3))
                if i < nsd and 0.0 <= a <= 1.0 and 0.0 < b <= 1.0:
                    refs.add(i)
            e["sounddefs"] = sorted(refs)
            e["waves"] = sorted({w for i in refs for w in self.sounddefs[i][1]}, key=str)

    def _last_category(self, a, b):
        d = self.d
        best = None
        for name in getattr(self, "cat_names", ()):
            s = struct.pack("<I", len(name) + 1) + name.encode() + b"\0"
            j = d.rfind(s, a, b)
            if j >= 0 and (best is None or j > best[0]):
                best = (j, name)
        return best[1] if best else None

    def block(self, e):
        return self.d[e["off"]:e["end"]]


def fmt_event(e):
    return ("%-34s cat=%-14s vol=%.3f (%+.2f dB) vrand=%g pitch=%g prand=%g prio=%d maxpb=%d mode=%#x "
            "min=%g max=%g" % (e["name"], e["category"], e["volume"], db(e["volume"]), e["volume_rand"],
                                e["pitch"], e["pitch_rand"], e["priority"], e["max_playbacks"], e["mode"],
                                e["min_dist"], e["max_dist"]))


def byte_diffs(a, b, limit=6):
    """Short description of where two byte strings differ."""
    if len(a) == len(b):
        offs = [i for i in range(len(a)) if a[i] != b[i]]
        if not offs:
            return []
        runs = []
        for o in offs:
            if runs and o <= runs[-1][1] + 4:
                runs[-1][1] = o
            else:
                runs.append([o, o])
        return ["+%#x: %s -> %s" % (s, a[s:e + 1].hex(), b[s:e + 1].hex()) for s, e in runs[:limit]]
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    out = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag != "equal":
            out.append("%s +%#x..%#x / +%#x..%#x (%d -> %d bytes)" % (tag, i1, i2, j1, j2, i2 - i1, j2 - j1))
    return out[:limit]


FIELDS = ("volume", "volume_rand", "pitch", "pitch_rand", "priority", "max_playbacks", "mode",
          "min_dist", "max_dist", "category")


def cmd_fev(args):
    def index(dirs):
        m = {}
        for d in dirs.split(","):     # several directories: "dir1,dir2"
            for p in sorted(glob.glob(os.path.join(d, "**", "*"), recursive=True)):
                if p.lower().endswith(".fev"):
                    m.setdefault(os.path.basename(p).lower(), p)
        return m

    pc, xb = index(args.pc), index(args.xb)
    pairs = sorted(set(pc) & set(xb))
    print("# FEV compare: %d PC files, %d Xbox files, %d pairs" % (len(pc), len(xb), len(pairs)))
    n_ident = 0
    n_ev = 0
    cat_issues = []
    ev_diffs = []
    blk_diffs = []
    only = []
    for k in pairs:
        a, b = Fev(pc[k]), Fev(xb[k])
        if a.d == b.d:
            n_ident += 1
        # categories
        ca = {c[0]: c for c in a.categories}
        cb = {c[0]: c for c in b.categories}
        for path in sorted(set(ca) | set(cb)):
            x, y = ca.get(path), cb.get(path)
            if not x or not y or x[1:] != y[1:]:
                cat_issues.append((k, path, x, y))
        for c in a.categories + b.categories:
            if c[1] != 1.0 or c[2] != 0.0:
                cat_issues.append((k, c[0], c, "non-unity"))
        # events, matched by GUID
        ea = {e["guid"]: e for e in a.events}
        eb = {e["guid"]: e for e in b.events}
        n_ev += len(set(ea) & set(eb))
        for g in sorted(set(ea) | set(eb)):
            x, y = ea.get(g), eb.get(g)
            if not x or not y:
                only.append((k, "PC" if x else "Xbox", (x or y)["name"], (x or y)["category"]))
                continue
            diff = [f for f in FIELDS if x[f] != y[f]]
            if diff:
                ev_diffs.append((k, x, y, diff))
            bd = byte_diffs(a.block(x), b.block(y))
            if bd:
                blk_diffs.append((k, x["name"], x["category"], bd))
    print("identical files: %d of %d; events matched by GUID: %d" % (n_ident, len(pairs), n_ev))
    print("\n## Category records (only differences or non-unity values)")
    for it in cat_issues or [("-", "none", None, None)]:
        print("  ", it)
    print("\n## Event header field differences (PC -> Xbox)")
    for k, x, y, diff in ev_diffs or []:
        print("  %s %s: %s" % (k, x["name"], ", ".join("%s %r -> %r" % (f, x[f], y[f]) for f in diff)))
    if not ev_diffs:
        print("   none")
    print("\n## Event blocks whose bytes differ (PC -> Xbox, offsets inside the block)")
    for k, name, cat, bd in blk_diffs:
        print("  %s %s [%s]: %s" % (k, name, cat, "; ".join(bd)))
    if not blk_diffs:
        print("   none")
    print("\n## Events present on one platform only")
    for it in only:
        print("  ", it)
    if not only:
        print("   none")
    if args.events:
        print("\n## All events (PC side)")
        for k in pairs:
            for e in Fev(pc[k]).events:
                print("  %s %s" % (k, fmt_event(e)))


def cmd_dump(args):
    f = Fev(args.file)
    print("version %#x, %d categories, %d events" % (f.version, len(f.categories), len(f.events)))
    for c in f.categories:
        print("  cat %-30s vol=%g pitch=%g extra=%s" % c)
    for e in f.events:
        print("  " + fmt_event(e))


# --------------------------------------------------------------------------------- DynamicMixer

def mixer_tables(path):
    """{section/preset: {group: dB}} for the group Volume entries of DynamicMixer.xml."""
    root = ET.parse(path).getroot()
    out = {}
    for sec in root:
        if sec.tag in ("AudioGroups",):
            continue
        if sec.tag == "Global":
            out["Global"] = dict(sec.attrib)
            continue
        for p in sec:
            vols = {}
            for g in p.iter():
                for v in g:
                    if v.tag == "Volume":
                        vols[g.tag.lower()] = float(v.get("Value"))
            out[sec.tag + "/" + p.tag] = vols
    return out


GROUPS = ("master", "music", "playerengine", "cpuengine", "swooshes", "surfaces", "powerplays", "collisions",
          "hud", "ambience", "particles", "spot_ambience", "hyperreal", "speech", "camera", "reverbsubmix")


def cmd_mixer(args):
    a, b = mixer_tables(args.pc), mixer_tables(args.xb)
    print("# DynamicMixer.xml compare (dB; PC vs Xbox)")
    same = True
    for k in sorted(set(a) | set(b)):
        x, y = a.get(k), b.get(k)
        if x != y:
            same = False
            print("  %s: PC %s | Xbox %s" % (k, x, y))
    if same:
        print("  identical")
    # The level a category plays at during a race: the active stack ("Game") plus the default preset
    # the code picks at audio init. Xbox: "Default" (surround) unless XAudioGetSpeakerConfig() == 1
    # (analog stereo only), then "XenonStereo". PC: always "Default" (the string is hard-coded).
    print("\n# In-race level per category, Game stack + default preset (dB, linear)")
    for name, t in (("PC", a), ("Xbox", b)):
        game = t.get("Stacks/Game", {})
        for preset in ("DefaultPresets/Default", "DefaultPresets/XenonStereo"):
            dp = t.get(preset, {})
            cells = []
            for g in GROUPS:
                v = game.get(g, 0.0) + dp.get(g, 0.0)
                cells.append("%s %+.1f (%.3f)" % (g, v, min(1.0, lin(v)) if g != "master" else lin(v)))
            print("  %-4s %-27s %s" % (name, preset, ", ".join(cells)))


# ------------------------------------------------------------------------------------------ FSB

def fsb_samples(path):
    d = open(path, "rb").read()
    if d[:4] != b"FSB4":
        raise ValueError("not FSB4")
    n, shdr = struct.unpack_from("<2I", d, 4)
    o, data, out = 48, 48 + shdr, []
    for _ in range(n):
        sz = struct.unpack_from("<H", d, o)[0]
        name = d[o + 2:o + 32].split(b"\0")[0].decode("latin1")
        nsamp, clen, _ls, _le, mode, freq = struct.unpack_from("<5Ii", d, o + 32)
        ch = struct.unpack_from("<H", d, o + 62)[0]
        out.append(dict(name=name, nsamp=nsamp, mode=mode, freq=freq, ch=ch, data=d[data:data + clen], index=len(out)))
        data += clen
        o += sz
    return out


def fsb_decode(s):
    """Decode one sample to float32 with ffmpeg (XMA wrapped in an XMA2 RIFF, MPEG raw, PCM16 raw)."""
    import numpy as np
    m, ch, freq = s["mode"], s["ch"], s["freq"]
    extra = []
    if m & 0x1000000:   # XMA
        ext = struct.pack("<HIIIIIIIBBH", 1, 4 if ch == 1 else 3, s["nsamp"], 0x10000, 0, s["nsamp"], 0, 0, 0, 4,
                          (len(s["data"]) + 0xFFFF) // 0x10000)
        fmt = struct.pack("<HHIIHHH", 0x166, ch, freq, freq * 2 * ch, 2048, 16, len(ext)) + ext
        riff = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(s["data"])) \
            + s["data"]
        blob, f = b"RIFF" + struct.pack("<I", len(riff)) + riff, "wav"
    elif m & 0x200:     # MPEG
        blob, f = s["data"], "mp3"
    elif m & 0x10:      # PCM16
        blob, f = s["data"], "s16le"
        extra = ["-ar", str(freq), "-ac", str(ch)]
    else:
        return None
    r = subprocess.run(["ffmpeg", "-v", "quiet", "-f", f] + extra + ["-i", "pipe:", "-f", "f32le", "pipe:"],
                       input=blob, capture_output=True)
    a = np.frombuffer(r.stdout, dtype="<f4")
    return a if a.size else None


def cmd_fsb(args):
    import numpy as np

    def level(a):
        a = a.astype(np.float64)
        return db(math.sqrt(float(np.mean(a * a))) + 1e-12), db(float(np.max(np.abs(a))) + 1e-12)

    rows = []
    for p in sorted(glob.glob(os.path.join(args.pc, "*.fsb"))):
        x = os.path.join(args.xb, os.path.basename(p))
        if not os.path.exists(x):
            continue
        xs = {s["name"]: s for s in fsb_samples(x)}
        for s in fsb_samples(p):
            t = xs.get(s["name"])
            if not t:
                continue
            a, b = fsb_decode(s), fsb_decode(t)
            if a is None or b is None:
                continue
            (ra, pa), (rb, pb) = level(a), level(b)
            rows.append((os.path.basename(p), s["name"], ra - rb, pa - pb, ra, rb, s["index"], len(a) // max(1, s["ch"])))
            print("%s %-40s rms PC-Xbox %+.2f dB  peak %+.2f dB" % rows[-1][:4], flush=True)
    if args.json:
        json.dump([dict(fsb=r[0], sample=r[1], index=r[6], rms_diff=r[2], peak_diff=r[3], rms_pc=r[4], rms_xb=r[5])
                   for r in rows], open(args.json, "w"), indent=0)
    if rows:
        d = sorted(r[2] for r in rows if r[4] > -60 and r[5] > -60)
        print("\n%d samples; RMS difference PC-Xbox: median %+.2f dB, 10%% %+.2f, 90%% %+.2f"
              % (len(d), d[len(d) // 2], d[len(d) // 10], d[9 * len(d) // 10]))


# ------------------------------------------------------------------------ console_mix table

# Event property indices of FMOD Designer 4.28 (FMOD_EVENT_PROPERTY), as the game's own name table lists them
# (PC image 0xD43580: "Volume" 1, "Volume randomization" 2, "Pitch" 3, "Pitch randomization" 7, "Priority" 11,
# "Max playbacks" 12, "3D Min distance" 17, "3D Max distance" 18).
PROPS = (("volume", 1), ("volume_rand", 2), ("pitch", 3), ("pitch_rand", 7), ("priority", 11), ("max_playbacks", 12),
         ("min_dist", 17), ("max_dist", 18))
INT_PROPS = (11, 12)  # FMOD int properties: the table still carries them as floats, the DLL reads and sets an int


def cfloat(v):
    """A C++ float literal."""
    t = "%.9g" % v
    return (t if ("." in t or "e" in t) else t + ".0") + "f"


def fnv1a(name):
    h = 0x811C9DC5
    for c in name.encode("latin1"):
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


SOUND_INSTANCE = re.compile(rb"(?=(..)(....)(....)\x00\x00\x00\x00[\x00\x01]\x00\x00\x00\xff\xff\xff\xff)", re.S)


def instance_volume_diffs(a, e, b, y):
    """Sound instances of one event (PC a/e, console b/y) whose volume differs and nothing else: the 42 PC bytes
    that hold it (sound definition, start, length, start/loop mode, loop count, autopitch, fine tune, volume), which
    must occur once in the PC file, and the two volumes. Record: u16 sound definition, f32 start, f32 length, u32
    start mode, u16 loop mode, u16 autopitch parameter, s32 loop count, u32 autopitch on, f32 autopitch reference,
    f32 autopitch at min, f32 fine tune, f32 volume, f32 fade in, f32 fade out, u32 fade in type, u32 fade out type."""
    def recs(f, ev):
        blk, out = f.d[ev["off"]:ev["end"]], []
        for m in SOUND_INSTANCE.finditer(blk):
            o = m.start()
            if o + 58 > len(blk) or struct.unpack_from("<H", blk, o)[0] >= len(f.sounddefs):
                continue  # not a sound instance (an envelope point that looks like one)
            out.append(blk[o:o + 58])
        return out
    ra, rb = recs(a, e), recs(b, y)
    if len(ra) != len(rb):
        return []
    out = []
    for u, v in zip(ra, rb):
        if u[:38] != v[:38] or u[42:] != v[42:] or u[38:42] == v[38:42]:
            continue
        if a.d.count(u[:42]) != 1:
            continue
        out.append((u[:42], a.sounddefs[struct.unpack_from("<H", u)[0]][0], struct.unpack_from("<f", u, 38)[0],
                    struct.unpack_from("<f", v, 38)[0], a.d.find(u[:42]) + 38))
    return out


def cmd_table(args):
    """Per-event corrections that give the PC the console's values: the event properties that differ, and for the
    volume, a gain for re-recorded samples where every sample the event can play was measured."""
    def index(dirs):
        m = {}
        for d in dirs.split(","):
            for p in sorted(glob.glob(os.path.join(d, "**", "*"), recursive=True)):
                if p.lower().endswith(".fev"):
                    m.setdefault(os.path.basename(p).lower(), p)
        return m

    pc, xb = index(args.pc), index(args.xb)
    levels = {}
    if args.levels:
        for r in json.load(open(args.levels)):
            levels.setdefault((r["sample"].lower(), r["index"]), []).append(r["rms_diff"])
    all_pc = []        # every PC event (name, property values), for the ambiguity check
    wanted = {}        # (name, prop index, pc value) -> (target, why)
    notes = []
    patches = []       # sound instance volumes: (project, event, sound definition, pattern bytes, pc, console)
    for k in sorted(set(pc) & set(xb)):
        a, b = Fev(pc[k]), Fev(xb[k])
        eb = {e["guid"]: e for e in b.events}
        for e in a.events:
            e["project"] = a.project
            all_pc.append(e)
            y = eb.get(e["guid"])
            if not y:
                continue
            for pat, sd, pv, xv, fo in instance_volume_diffs(a, e, b, y):
                patches.append((a.project, e["name"], sd, pat, pv, xv, fo, len(a.d)))
            gain_db, why = 0.0, ""
            if levels and e["waves"]:
                ds = []
                for w in e["waves"]:
                    fn, bank, idx = w
                    if bank is None:
                        ds = None  # an oscillator: nothing measured
                        break
                    v = levels.get((os.path.basename(fn.replace("\\", "/")).lower(), idx))
                    if not v or max(v) - min(v) > 0.5:
                        ds = None
                        break
                    ds.append(v[0])
                if ds and max(ds) - min(ds) <= args.spread and abs(sum(ds) / len(ds)) >= args.min_db:
                    gain_db = -sum(ds) / len(ds)
                    why = "samples %+.1f dB (%d)" % (-gain_db, len(ds))
            for field, prop in PROPS:
                pv, xv = e[field], y[field]
                target = xv
                if field == "volume" and gain_db:
                    target = xv * lin(gain_db)
                if abs(target - pv) < 1e-6:
                    continue
                if field == "volume" and target > 1.0:
                    notes.append("%s %s: volume %.3f wanted, clamped to 1.0" % (k, e["name"], target))
                    target = 1.0
                    if abs(target - pv) < 1e-6:
                        continue
                reason = "event %s %r -> %r" % (field, pv, xv) if pv != xv else ""
                if field == "volume" and why:
                    reason = (reason + "; " if reason else "") + why
                key = (a.project, e["name"], prop, struct.unpack("<I", struct.pack("<f", pv))[0])
                old = wanted.get(key)
                if old and (old[0] is None or abs(old[0] - target) > 1e-6):
                    wanted[key] = (None, "conflict", old[2] + 1)
                elif old:
                    wanted[key] = (old[0], old[1], old[2] + 1)
                else:
                    wanted[key] = (target, "%s %s: %s" % (k, e["name"], reason), 1)
            for f in ("mode",):
                if e[f] != y[f]:
                    notes.append("%s %s: %s PC %r console %r (not a run-time property; left)" % (k, e["name"], f, e[f], y[f]))
    # a key must not also catch a PC event that should stay as it is
    rows, dropped = [], []
    for (proj, name, prop, bits), (target, why, nsrc) in sorted(wanted.items()):
        field = [f for f, p in PROPS if p == prop][0]
        pv = struct.unpack("<f", struct.pack("<I", bits))[0]
        same = [e for e in all_pc if e["project"] == proj and e["name"] == name and
                struct.unpack("<I", struct.pack("<f", e[field]))[0] == bits]
        if target is None:
            dropped.append("%s prop %d: different console values for the same PC value" % (name, prop))
            continue
        if len(same) != nsrc:
            dropped.append("%s prop %d %.4f: %d PC events share name and value, only %d of them differ (%s)"
                           % (name, prop, pv, len(same), nsrc, why))
            continue
        rows.append((fnv1a(name), prop, pv, target, name, why, len(same), fnv1a(proj), proj))
    print("# %d corrections, %d dropped" % (len(rows), len(dropped)))
    for r in rows:
        print("  %-16s %-26s prop %2d  %.4f -> %.4f  (%+.1f dB)  %s" % (r[8], r[4], r[1], r[2], r[3],
              db(r[3]) - db(r[2]) if r[1] == 1 and r[2] > 0 else 0.0, r[5]))
    print("# %d sound instance volumes" % len(patches))
    for pr, ev, sd, _pat, pv, xv, _fo, _n in patches:
        print("  %-16s %-26s %-30s instance volume %.3f -> %.3f (%+.1f dB)" % (pr, ev, sd, pv, xv, db(xv) - db(pv)))
    for d in dropped:
        print("  dropped: " + d)
    for n in notes:
        print("  note: " + n)
    if args.header:
        with open(args.header, "w") as h:
            h.write("// Generated by tools/audio/fev_compare.py table: PC event values that differ from the Xbox 360 release,\n"
                    "// and the console value (event volume times a gain for re-recorded samples). Keyed by the FNV-1a hash\n"
                    "// of the project and of the event name, the FMOD event property index and the PC value it must have (so that only the\n"
                    "// event meant is touched). Numbers only.\n#pragma once\n#include <cstdint>\n\nnamespace sscm {\n\n"
                    "struct Correction {\n  uint32_t project_hash;\n  uint32_t name_hash;\n  int prop;\n  float pc;\n  float console;\n};\n\n"
                    "constexpr Correction kCorrections[] = {\n")
            for r in rows:
                h.write("    {0x%08X, 0x%08X, %d, %s, %s},\n" % (r[7], r[0], r[1], cfloat(r[2]), cfloat(r[3])))
            h.write("};\nconstexpr int kCorrectionCount = sizeof(kCorrections) / sizeof(kCorrections[0]);\n\n")
            h.write("// Sound instance volumes (no run-time property): patched in the project's .fev as FMOD reads it. The file\n"
                    "// (FNV-1a of the lower-case project name, i.e. its file name without .fev) and its size, the offset of the\n"
                    "// volume float, the PC and the console volume.\n"
                    "struct InstancePatch {\n  uint32_t file_hash;\n  uint32_t file_size;\n  uint32_t offset;\n  float pc;\n  float console;\n};\n\n"
                    "constexpr InstancePatch kInstancePatches[] = {\n")
            for pr, ev, sd, pat, pv, xv, fo, n in patches:
                h.write("    {0x%08X, %d, %d, %s, %s},  // %s %s %s\n" % (fnv1a(pr.lower()), n, fo, cfloat(pv), cfloat(xv), pr, ev, sd))
            h.write("};\nconstexpr int kInstancePatchCount = sizeof(kInstancePatches) / sizeof(kInstancePatches[0]);\n\n"
                    "}  // namespace sscm\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("fev")
    p.add_argument("pc")
    p.add_argument("xb")
    p.add_argument("--events", action="store_true", help="also list every PC event")
    p.set_defaults(fn=cmd_fev)
    p = sub.add_parser("table")
    p.add_argument("pc")
    p.add_argument("xb")
    p.add_argument("--levels", help="JSON from `fsb --json`: adds the sample-level gains")
    p.add_argument("--spread", type=float, default=3.0, help="max spread of an event's sample deltas, dB")
    p.add_argument("--min-db", type=float, default=1.0, help="smallest sample delta worth a correction, dB")
    p.add_argument("--header", help="write the C++ table here")
    p.set_defaults(fn=cmd_table)
    p = sub.add_parser("dump")
    p.add_argument("file")
    p.set_defaults(fn=cmd_dump)
    p = sub.add_parser("mixer")
    p.add_argument("pc")
    p.add_argument("xb")
    p.set_defaults(fn=cmd_mixer)
    p = sub.add_parser("fsb")
    p.add_argument("pc")
    p.add_argument("xb")
    p.add_argument("--json", help="write the per-sample results here (input of `table`)")
    p.set_defaults(fn=cmd_fsb)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
