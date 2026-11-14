#!/usr/bin/env python3
"""ab_analyze.py: channel levels of two multichannel recordings side by side, PC against console.

  ab_analyze.py --pc pc/cap.wav [--pc-log pc/splitsecond-patch.log --pc-start pc/cap.start] --console rx/cap.wav
  ab_analyze.py --pc a.wav --console b.wav --from 60 --to 160      only that part of both (seconds)

Recordings: float32 or 16-bit PCM WAV, 2, 6 (FL FR FC LFE RL RR) or 8 channels (+ SL SR), as tools/audio/ab_capture.sh
writes them. The two need not be the same drive: every figure is a statistic over the recording (levels against the
front pair, percentiles, means over the transients found), so the same track and a comparable stretch are enough.

Printed, per recording and as PC minus console:
  levels      RMS of the whole part per channel group (front pair, centre, LFE, rears, sides), in dBFS and against front
  floor       how low each group gets while the front pair is loud (percentiles over 100 ms windows): something that is
              always in a speaker (a reverb return, an engine send) shows as a floor
  transients  windows that jump 9 dB over the last 2 s (impacts, power plays): how many, the mean jump, where they sit
  tails       the 0.2-0.8 s and 0.8-2.0 s after each transient against its peak (reverb energy), per group, and how
              alike centre and rears are there (a mono return in every speaker gives a correlation near 1)
  categories  (PC, with --pc-log from an audio_diag run and --pc-start) per event category of the [start] lines: how
              many sounds started, and where the 0.3 s after a start sits (rear, centre, LFE against front)
"""
import argparse, math, re, struct, sys
import numpy as np

GROUPS = [("front", (0, 1)), ("centre", (2,)), ("LFE", (3,)), ("rear", (4, 5)), ("side", (6, 7))]


def db(x):
    return 20 * math.log10(max(float(x), 1e-9))


def load(path):
    w = open(path, "rb").read()
    i = w.find(b"fmt ")
    fmt, ch, sr, _, _, bits = struct.unpack_from("<HHIIHH", w, i + 8)
    j = w.find(b"data", i)
    raw = w[j + 8:]
    if fmt == 3 or (fmt == 0xFFFE and bits == 32):
        d = np.frombuffer(raw[: len(raw) // (4 * ch) * 4 * ch], dtype="<f4")
    elif bits == 16:
        d = np.frombuffer(raw[: len(raw) // (2 * ch) * 2 * ch], dtype="<i2").astype(np.float32) / 32768.0
    else:
        sys.exit("%s: format %d with %d bits not understood" % (path, fmt, bits))
    return d.reshape(-1, ch), sr


def group_rms(r, idx):
    idx = [k for k in idx if k < r.shape[-1]]
    if not idx:
        return None
    return np.sqrt((r[..., idx] ** 2).mean(-1))


class Rec:
    def __init__(self, name, path, t0, t1):
        self.name = name
        d, sr = load(path)
        self.sr = sr
        self.offset = t0
        d = d[int(t0 * sr): int(t1 * sr) if t1 else None]
        self.d = d
        self.ch = d.shape[1]
        self.win = sr // 10
        m = len(d) // self.win
        self.r = np.sqrt((d[: m * self.win].reshape(m, self.win, self.ch) ** 2).mean(1)) + 1e-9
        self.tot = np.sqrt((self.r ** 2).sum(1))
        self.g = {n: group_rms(self.r, idx) for n, idx in GROUPS}

    def levels(self):
        out = {}
        rms = np.sqrt((self.d ** 2).mean(0)) + 1e-9
        for n, idx in GROUPS:
            v = group_rms(rms, idx)
            if v is not None:
                out[n] = db(v)
        return out

    def floors(self):
        front = self.g["front"]
        act = front > 10 ** (-30 / 20)
        out = {}
        for n, _ in GROUPS[1:]:
            if self.g[n] is None or not act.any():
                continue
            rel = 20 * np.log10(self.g[n][act] / front[act])
            out[n] = np.percentile(rel, [1, 5, 50])
        return out, int(act.sum())

    def transients(self):
        m = len(self.tot)
        ev = [t for t in range(20, m - 20) if db(self.tot[t]) - db(np.median(self.tot[max(0, t - 20): t])) > 9]
        runs = []
        for t in ev:
            if not runs or t - runs[-1][-1] > 10:
                runs.append([t])
            else:
                runs[-1].append(t)
        peaks = [max(e, key=lambda q: self.tot[q]) for e in runs]
        res = {"n": len(peaks)}
        if not peaks:
            return res
        res["jump"] = float(np.mean([db(self.tot[t]) - db(np.median(self.tot[max(0, t - 20): t])) for t in peaks]))
        for n, _ in GROUPS[1:]:
            if self.g[n] is None:
                continue
            res[n] = float(np.mean([db(self.g[n][t]) - db(self.g["front"][t]) for t in peaks]))
            for lab, a, b in (("tail1", 2, 8), ("tail2", 8, 20)):
                res[n + "_" + lab] = float(np.mean([db(np.sqrt((self.g[n][t + a: t + b] ** 2).mean())) -
                                                    db(self.g["front"][t]) for t in peaks]))
        for lab, a, b in (("tail1", 2, 8), ("tail2", 8, 20)):
            res["front_" + lab] = float(np.mean([db(np.sqrt((self.g["front"][t + a: t + b] ** 2).mean())) -
                                                 db(self.g["front"][t]) for t in peaks]))
        if self.ch >= 6:
            cc = []
            for t in peaks:
                seg = self.d[(t + 2) * self.win: (t + 20) * self.win]
                if len(seg) and seg[:, 2].std() > 1e-7 and seg[:, 4].std() > 1e-7:
                    cc.append(np.corrcoef(seg[:, 2], seg[:, 4])[0, 1])
            if cc:
                res["corr"] = float(np.mean(cc))
        return res

    def categories(self, log, start):
        def secs(s):
            h, m, x = s.split(":")
            return int(h) * 3600 + int(m) * 60 + float(x)
        t0 = secs(open(start).read().split()[0][:12]) + self.offset
        cats = {}
        for line in open(log, errors="replace"):
            m = re.match(r"(\d\d:\d\d:\d\d\.\d+) \[start\] t=\d+ (\S+) cat=(\S+)", line)
            if not m:
                continue
            w = int((secs(m.group(1)) - t0) * 10)
            if w < 3 or w + 3 >= len(self.tot):
                continue
            row = [db(np.sqrt((self.tot[w: w + 3] ** 2).mean())) - db(np.sqrt((self.tot[w - 3: w] ** 2).mean()))]
            f = np.sqrt((self.g["front"][w: w + 3] ** 2).mean())
            for n in ("rear", "centre", "LFE"):
                row.append(db(np.sqrt((self.g[n][w: w + 3] ** 2).mean())) - db(f) if self.g[n] is not None else float("nan"))
            cats.setdefault(m.group(3), []).append(row)
        return cats


def fmt(v):
    return "   n/a " if v is None or (isinstance(v, float) and math.isnan(v)) else "%7.1f" % v


def table(title, rows, recs):
    print("\n" + title)
    print("  %-34s" % "" + "".join("%9s" % r.name for r in recs) + ("%9s" % "PC-cons" if len(recs) == 2 else ""))
    for label, vals in rows:
        d = vals[0] - vals[1] if len(vals) == 2 and None not in vals else None
        print("  %-34s" % label + "".join("  " + fmt(v) for v in vals) + ("  " + fmt(d) if len(recs) == 2 else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pc")
    ap.add_argument("--console")
    ap.add_argument("--pc-log")
    ap.add_argument("--pc-start")
    ap.add_argument("--from", dest="t0", type=float, default=0.0)
    ap.add_argument("--to", dest="t1", type=float, default=0.0)
    a = ap.parse_args()
    recs = [Rec(n, p, a.t0, a.t1) for n, p in (("PC", a.pc), ("console", a.console)) if p]
    if not recs:
        ap.error("give --pc and/or --console")
    for r in recs:
        print("%s: %d channels, %.0f s analysed" % (r.name, r.ch, len(r.d) / r.sr))

    lv = [r.levels() for r in recs]
    rows = [("front pair, dBFS", [l.get("front") for l in lv])]
    rows += [("%s minus front, dB" % n, [l[n] - l["front"] if n in l else None for l in lv]) for n, _ in GROUPS[1:]]
    table("levels (RMS of the whole part)", rows, recs)

    fl = [r.floors() for r in recs]
    rows = []
    for n, _ in GROUPS[1:]:
        for k, lab in enumerate(("1 %", "5 %", "median")):
            rows.append(("%s minus front, %s" % (n, lab), [f[0][n][k] if n in f[0] else None for f in fl]))
    table("floor while the front pair is above -30 dBFS (100 ms windows: %s)" % ", ".join(str(f[1]) for f in fl), rows, recs)

    tr = [r.transients() for r in recs]
    rows = [("transients found", [float(t["n"]) for t in tr]), ("mean jump over the last 2 s, dB", [t.get("jump") for t in tr])]
    rows += [("%s minus front at the peak" % n, [t.get(n) for t in tr]) for n, _ in GROUPS[1:]]
    table("transients (9 dB over the median of the last 2 s)", rows, recs)

    rows = []
    for lab, what in (("tail1", "0.2-0.8 s"), ("tail2", "0.8-2.0 s")):
        for n, _ in GROUPS:
            rows.append(("%s %s after, vs front peak" % (n, what), [t.get(n + "_" + lab) for t in tr]))
    rows.append(("centre/rear-left correlation, tail", [t.get("corr") for t in tr]))
    table("tails after the transients (reverb energy)", rows, recs)

    if a.pc and a.pc_log and a.pc_start:
        cats = recs[0].categories(a.pc_log, a.pc_start)
        print("\ncategories (PC: the 0.3 s after each [start] line of the audio_diag log; medians)")
        print("  %-16s %6s %9s %9s %9s %9s" % ("category", "starts", "jump", "rear-fr", "centre-fr", "LFE-fr"))
        for c in sorted(cats, key=lambda k: -len(cats[k])):
            v = np.array(cats[c])
            med = np.nanmedian(v, 0)
            print("  %-16s %6d %9.1f %9.1f %9.1f %9.1f" % (c, len(v), med[0], med[1], med[2], med[3]))
        if not cats:
            print("  no [start] line falls inside the recording (audio_diag = true? cap.start of the same run?)")


if __name__ == "__main__":
    main()
