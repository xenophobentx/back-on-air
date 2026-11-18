"""Usage statistics and the two web pages (public, owner), written as static files for a web server to serve.

The design is the Takedawn lobby's (Burnout 3, docs/online.md "Statistics", "Public page", "Owner's page"): rdvd
serves no HTTP. It keeps one dict of counts per UTC day and writes files:

  stats dir (private, with the database):
    daily.jsonl     one JSON line per finished UTC day, appended at midnight (or at the first start after it)
    current.json    today so far, every minute and at stop; a restart on the same day carries on from it
  public dir (world-readable, for Caddy):
    index.html      the public page (web/stats_page.html)
    stats.json      every 20 s: players and games now, today, the last 30 days, the open games (neutral rows)
    <token>/        with --admin-token-file: the owner's page (web/admin_page.html), now.json, history.jsonl

Only counts are kept. No name, no address, no Steam data: the server never gets a SteamID or a Steam account
name, and the public files hold no player name either (the open games list has no game name, because the game
names a room after its host's online name). current.json keeps today's account numbers (pids, the database's own
keys) so a restart does not count a player twice; it lives in the private stats dir and is dropped at midnight.

What the game tells the server about a room (read from the image, docs/plan-online.md 1.4 and 12):
  attribute 0  1 = public, 2 = private (the join code refuses "Private session, we have no permission to join"
               when it is 2, 0xA29BCC). A public PUBLIC GAME -> RACE room was captured with 1.
  attribute 1  the EVENT number the quick-match search filters on (EVENT_MIN..EVENT_MAX, 0xA29C31). Which number
               is which mode is not known yet: MODE_NAMES stays empty until a capture of each mode fills it.
  race start   none. XSessionStart and XSessionModify are `xor eax,eax; ret` on PC (0xA36C90), and the first
               online race (plan 12.2) sent no call from the race start to the results; even the menus' Friends
               poll (~4 calls a second) stops. --race-detect turns on a heuristic built on that: a room with two
               or more players whose host has made no call for RACE_QUIET seconds is racing. It is off by default
               until a live run with several races confirms it ("pending live test" on the pages).
"""
import json
import logging
import os
import re
import struct
import time

log = logging.getLogger("rdvd")

PUBLIC_EVERY = 20          # s: stats.json and the owner's now.json
CURRENT_EVERY = 60         # s: current.json
RACE_QUIET = 10.0          # s without a call from a room's host (two or more players) = a race is under way
MAX_LISTED = 50
PUBLIC = 1                 # attribute 0
MODE_NAMES = {}            # attribute 1 (EVENT) -> mode name; fill after a capture of each mode (pending live test)
TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{20,64}$")
WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")

# server.counters that become day counts: counter -> field
DELTAS = {"refused_full": "refused_full", "relay_datagrams": "relay_packets", "relay_bytes": "relay_bytes",
          "relay_registrations": "relay_registrations", "relay_dropped": "relay_dropped",
          "relay_games": "relay_games", "relay_peer_answers": "relay_peer_answers"}
DELTAS.update({"relay_drop_" + r: "relay_drop_" + r for r in ("bad", "tag", "member", "nodest", "rate", "full", "taken")})
ALONE_BUCKETS = ((60, "lt1m"), (300, "lt5m"), (900, "lt15m"), (None, "ge15m"))
HOST_LEFT_HOW = {"unregistered": "close", "owner registered another": "moved", "owner disconnected": "lost"}


def utc_date(t: float) -> str:
    return time.strftime("%Y-%m-%d", time.gmtime(t))


def blank_day(date: str) -> dict:
    d = {"date": date, "signins": 0, "unique_players": 0, "returning_players": 0, "peak_players": 0,
         "peak_sessions": 0, "hour_peak": [0] * 24, "games_created": 0, "games_public": 0, "games_private": 0,
         "race_detect": False, "games_started": 0, "closed_alone": 0, "closed_alone_never": 0, "closed_alone_secs": 0,
         "host_left": 0, "host_left_players": 0, "countries": {}}
    for _, k in ALONE_BUCKETS:
        d["closed_alone_" + k] = 0
    for k in HOST_LEFT_HOW.values():
        d["host_left_" + k] = 0
    for p in range(1, 9):
        d["start_players_%d" % p] = 0
    for k in DELTAS.values():
        d[k] = 0
    return d


def attributes(g) -> tuple:
    """The ten u32 after the base Gathering (zeros where the tail is short)."""
    t = (g.tail or b"")[:40]
    t += b"\0" * (40 - len(t))
    return struct.unpack("<10I", t)


def is_private(g) -> bool:
    return attributes(g)[0] != PUBLIC        # anything but a plain public room stays off the public list


def event_of(g) -> int:
    return attributes(g)[1]


def mode_name(ev: int) -> str:
    return MODE_NAMES.get(ev, "")


def write_file(path: str, data, mode: int):
    """Atomic write with an explicit mode (the unit's UMask=0077 would make a plain open() 0600)."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    tmp = path + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
    try:
        os.fchmod(fd, mode)
        os.write(fd, data)
    finally:
        os.close(fd)
    os.replace(tmp, path)


def compact(d: dict) -> str:
    return json.dumps(d, separators=(",", ":"), sort_keys=False)


def read_token(path: str) -> str:
    with open(path, encoding="ascii") as f:
        token = f.readline().strip()
    if not TOKEN_RE.match(token):
        raise ValueError("the admin token file must hold one line of 20-64 letters, digits, '-' or '_'")
    return token


class Stats:
    def __init__(self, stats_dir=None, public_dir=None, admin_token=None, geoip=None, race_detect=False,
                 docs_url="", clock=time.time):
        self.stats_dir, self.admin_token, self.geoip = stats_dir, admin_token, geoip
        self.public_dir = public_dir or (os.path.join(stats_dir, "public") if stats_dir else None)
        self.race_detect, self.docs_url, self.clock = race_detect, docs_url, clock
        self.server = None
        self.started = clock()
        self.history = []          # finished days, oldest first
        self.seen = set()          # pids signed in today
        self.carry = {}            # today's delta counts from before a restart
        self.base = {}             # server.counters at the start of the day (or of this run)
        self.last_public = self.last_current = 0.0
        self.day = blank_day(utc_date(self.started))
        if stats_dir:
            os.makedirs(stats_dir, exist_ok=True)
            self._load()
        self.day["race_detect"] = self.day["race_detect"] or race_detect

    def bind(self, server):
        self.server = server
        self.base = dict(server.counters)

    # ------------------------------------------------------------------------------------------- files: state
    def _path(self, name):
        return os.path.join(self.stats_dir, name)

    def _load(self):
        try:
            with open(self._path("daily.jsonl"), encoding="utf-8") as f:
                for line in f:
                    try:
                        self.history.append(json.loads(line))
                    except ValueError:
                        pass
        except FileNotFoundError:
            pass
        try:
            with open(self._path("current.json"), encoding="utf-8") as f:
                cur = json.load(f)
        except (FileNotFoundError, ValueError):
            return
        seen = cur.pop("seen", [])
        for k in ("sessions_now", "players_now"):
            cur.pop(k, None)
        day = blank_day(cur.get("date", ""))
        day.update(cur)
        if day["date"] == self.day["date"]:
            self.day, self.seen = day, set(int(p) for p in seen)
            self.carry = {k: day.get(k, 0) for k in DELTAS.values()}
        elif day["date"] and (not self.history or self.history[-1].get("date") < day["date"]):
            self._append_daily(day)       # the server was down at that midnight

    def _append_daily(self, day: dict):
        self.history.append(day)
        if self.stats_dir:
            with open(self._path("daily.jsonl"), "a", encoding="utf-8") as f:
                f.write(compact(day) + "\n")

    def _save_current(self):
        if not self.stats_dir:
            return
        cur = dict(self.day)
        cur["sessions_now"], cur["players_now"] = self._sessions(), self._players()
        cur["seen"] = sorted(self.seen)
        write_file(self._path("current.json"), compact(cur), 0o600)

    # ------------------------------------------------------------------------------------------- the day
    def _sync(self):
        """Today's counts that come from server.counters."""
        if self.server is None:
            return
        c = self.server.counters
        for src, dst in DELTAS.items():
            self.day[dst] = self.carry.get(dst, 0) + c.get(src, 0) - self.base.get(src, 0)

    def _rollover(self, date: str):
        self._sync()
        self._append_daily(self.day)
        self.day = blank_day(date)
        self.day["race_detect"] = self.race_detect
        self.seen, self.carry = set(), {}
        self.base = dict(self.server.counters) if self.server else {}
        self._write_history()

    def _check_day(self):
        date = utc_date(self.clock())
        if date != self.day["date"]:
            self._rollover(date)

    def _players(self):
        return len(self.server.sessions) if self.server else 0

    def _sessions(self):
        if not self.server:
            return 0
        return sum(1 for c in self.server.secure.conns.values() if c.connected)

    def _peaks(self):
        p, s, d = self._players(), self._sessions(), self.day
        d["peak_players"] = max(d["peak_players"], p)
        d["peak_sessions"] = max(d["peak_sessions"], s)
        h = time.gmtime(self.clock()).tm_hour
        d["hour_peak"][h] = max(d["hour_peak"][h], p)

    # ------------------------------------------------------------------------------------------- hooks
    def signin(self, pid: int, ip: str, created_day: int):
        """A real account finished the secure handshake (the guest login is not counted)."""
        self._check_day()
        d = self.day
        d["signins"] += 1
        cc = self.geoip.country(ip) if self.geoip else "??"
        u_s = d["countries"].setdefault(cc, [0, 0])
        u_s[1] += 1
        if pid not in self.seen:
            self.seen.add(pid)
            d["unique_players"] = len(self.seen)
            u_s[0] += 1
            if created_day is not None and created_day < int(self.clock()) // 86400:
                d["returning_players"] += 1
        self._peaks()

    def gathering_registered(self, g):
        self._check_day()
        g.registered_at = self.clock()
        d = self.day
        d["games_created"] += 1
        if is_private(g):
            d["games_private"] += 1
        else:
            d["games_public"] += 1
        k = "created_event_%d" % event_of(g)
        d[k] = d.get(k, 0) + 1

    def gathering_removed(self, g, why: str):
        self._check_day()
        if g.races:
            return
        d, others = self.day, [p for p in g.participants if p != g.owner]
        if not others:
            d["closed_alone"] += 1
            if not g.joined:
                d["closed_alone_never"] += 1
            secs = max(0, int(self.clock() - getattr(g, "registered_at", self.clock())))
            d["closed_alone_secs"] += secs
            for limit, k in ALONE_BUCKETS:
                if limit is None or secs < limit:
                    d["closed_alone_" + k] += 1
                    break
        else:
            d["host_left"] += 1
            d["host_left_players"] += len(others)
            how = HOST_LEFT_HOW.get(why)
            if how:
                d["host_left_" + how] += 1

    def race_started(self, g):
        d = self.day
        d["games_started"] += 1
        k = "started_event_%d" % event_of(g)
        d[k] = d.get(k, 0) + 1
        n = max(1, min(8, len(g.participants)))
        d["start_players_%d" % n] += 1

    def _detect_races(self, now: float):
        for g in self.server.gatherings.values():
            owner = self.server.sessions.get(g.owner)
            if owner is None:
                continue
            quiet = now - getattr(owner, "last_call", owner.created) >= RACE_QUIET
            if not g.racing and quiet and len(g.participants) >= 2:
                g.racing = True
                g.races += 1
                self.race_started(g)
            elif g.racing and not quiet:
                g.racing = False

    def tick(self, now: float):
        """now: the server's monotonic clock (the connections' times); the day uses self.clock()."""
        self._check_day()
        if self.race_detect and self.server:
            self._detect_races(now)
        self._peaks()
        t = self.clock()
        if t - self.last_public >= PUBLIC_EVERY:
            self.last_public = t
            self.write_public()
        if t - self.last_current >= CURRENT_EVERY:
            self.last_current = t
            self._save_current()

    def close(self):
        self._sync()
        self._save_current()
        self.write_public()

    # ------------------------------------------------------------------------------------------- what is shown
    def now_counts(self) -> dict:
        gs = list(self.server.gatherings.values()) if self.server else []
        private = [g for g in gs if is_private(g)]
        public = [g for g in gs if not is_private(g)]
        racing = [g for g in gs if g.racing] if self.race_detect else None
        return {"players": self._players(), "sessions": self._sessions(),
                "games": len(gs), "games_private": len(private),
                "games_waiting": len([g for g in public if not g.racing]),
                "games_racing": len(racing) if racing is not None else None}

    def games_list(self) -> list:
        """Public rooms only, neutral rows: mode, players, limit, state. No name, no id."""
        gs = [g for g in (self.server.gatherings.values() if self.server else []) if not is_private(g)]
        gs.sort(key=lambda g: (g.racing, -len(g.participants), g.gid))
        return [{"mode": mode_name(event_of(g)), "players": len(g.participants), "max": g.max,
                 "state": "racing" if g.racing else "waiting"} for g in gs[:MAX_LISTED]]

    def by_mode(self, d: dict) -> list:
        evs = sorted({int(k.split("_")[-1]) for k in d if k.startswith(("created_event_", "started_event_"))})
        return [{"event": e, "mode": mode_name(e) or "Other", "created": d.get("created_event_%d" % e, 0),
                 "started": d.get("started_event_%d" % e, 0) if d.get("race_detect") else None} for e in evs]

    @staticmethod
    def _short(d: dict) -> dict:
        return {"date": d["date"], "unique_players": d.get("unique_players", 0), "signins": d.get("signins", 0),
                "games_created": d.get("games_created", 0),
                "games_started": d.get("games_started", 0) if d.get("race_detect") else None}

    def public_json(self) -> dict:
        self._sync()
        d = self.day
        today = self._short(d)
        today.update({"peak_players": d["peak_players"], "by_mode": self.by_mode(d) if MODE_NAMES else []})
        return {"updated": int(self.clock()), "started": int(self.started), "race_detect": self.race_detect,
                "modes_known": bool(MODE_NAMES), "now": self.now_counts(), "today": today,
                "days": [self._short(x) for x in self.history[-29:]], "games": self.games_list()}

    def owner_json(self) -> dict:
        self._sync()
        today = dict(self.day)
        return {"updated": int(self.clock()), "started": int(self.started), "race_detect": self.race_detect,
                "mode_names": {str(k): v for k, v in MODE_NAMES.items()}, "now": self.now_counts(), "today": today,
                "geo_ranges": len(self.geoip) if self.geoip else 0}

    # ------------------------------------------------------------------------------------------- files: web
    def _page(self, name: str) -> str:
        with open(os.path.join(WEB, name), encoding="utf-8") as f:
            page = f.read()
        return page.replace("{{DOCS_URL}}", json.dumps(self.docs_url or ""))

    def _owner_dir(self):
        return os.path.join(self.public_dir, self.admin_token) if self.admin_token else None

    def _write_history(self):
        od = self._owner_dir() if self.public_dir else None
        if od and os.path.isdir(od):
            write_file(os.path.join(od, "history.jsonl"), "".join(compact(x) + "\n" for x in self.history), 0o644)

    def write_pages(self):
        """At start: the folders and the two pages (the data files follow every 20 s)."""
        if not self.public_dir:
            return
        os.makedirs(self.public_dir, exist_ok=True)
        os.chmod(self.public_dir, 0o711 if self.admin_token else 0o755)    # 0711: nobody can list the token folder
        write_file(os.path.join(self.public_dir, "index.html"), self._page("stats_page.html"), 0o644)
        fonts = os.path.join(self.public_dir, "fonts")         # Barlow Condensed (OFL), served from here: no font CDN
        os.makedirs(fonts, exist_ok=True)
        os.chmod(fonts, 0o755)
        for name in sorted(os.listdir(os.path.join(WEB, "fonts"))):
            with open(os.path.join(WEB, "fonts", name), "rb") as f:
                write_file(os.path.join(fonts, name), f.read(), 0o644)
        od = self._owner_dir()
        if od:
            os.makedirs(od, exist_ok=True)
            os.chmod(od, 0o755)
            write_file(os.path.join(od, "index.html"), self._page("admin_page.html"), 0o644)
            self._write_history()
        log.info("web pages written%s", " (with the owner's page)" if od else "")

    def write_public(self):
        if not self.public_dir:
            return
        try:
            write_file(os.path.join(self.public_dir, "stats.json"), compact(self.public_json()), 0o644)
            od = self._owner_dir()
            if od:
                write_file(os.path.join(od, "now.json"), compact(self.owner_json()), 0o644)
        except OSError as e:
            log.info("could not write the web files: %s", e.strerror or e)
