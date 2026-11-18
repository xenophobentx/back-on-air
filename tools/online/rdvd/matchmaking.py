"""MatchMaking (protocol 21): gatherings in memory.

The game's session type is "ATVGameSession": the library's Gathering followed by ten u32 the server does not need
to understand (the client filters search results itself), so everything after the description is kept as bytes.

Gathering on the wire, inside an AnyDataHolder:
    u32 id, u32 owner pid, u32 host pid, u16 min participants, u16 max participants, u32 participation policy,
    u32 policy argument, u32 flags, u32 state, String description, <type-specific tail>
"""
import logging
import time

from . import codec
from .codec import Reader, Writer
from .secure import _station_urls
from .server import rmc, RmcError

log = logging.getLogger("rdvd")
MAX_TAIL = 256
MAX_ANSWER_URLS = 48     # GetParticipantsURLs: 8 players x (public + up to 5 registered); fragments carry it
MAX_RESULTS = 20          # gatherings in one answer (keeps the answer inside one packet)
NOTIFY_JOIN, NOTIFY_LEAVE, NOTIFY_GONE = 3001, 3002, 109000


class Gathering:
    def __init__(self):
        self.gid = self.owner = self.host = 0
        self.min = self.max = 0
        self.policy = self.policy_arg = self.flags = self.state = 0
        self.description = ""
        self.type = ""
        self.tail = b""
        self.participants = []      # pids, in joining order
        self.session_url = ""
        self.created = time.time()
        self.racing = False         # stats.py's race-start heuristic (--race-detect)
        self.races = 0
        self.joined = False         # somebody other than the owner took part

    @classmethod
    def parse(cls, type_name: str, body: bytes):
        g, r = cls(), Reader(body)
        g.type = type_name
        g.gid, g.owner, g.host = r.u32(), r.u32(), r.u32()
        g.min, g.max = r.u16(), r.u16()
        g.policy, g.policy_arg, g.flags, g.state = r.u32(), r.u32(), r.u32(), r.u32()
        g.description = r.string(256)
        g.tail = r.rest()
        if len(g.tail) > MAX_TAIL or not 0 < len(type_name) <= 32:
            raise codec.WireError("gathering too large")
        return g

    def put(self, w: Writer):
        body = (Writer().u32(self.gid).u32(self.owner).u32(self.host).u16(self.min).u16(self.max).u32(self.policy)
                .u32(self.policy_arg).u32(self.flags).u32(self.state).string(self.description).raw(self.tail).bytes())
        w.anydata(self.type, body)


def _remove(server, g, why: str):
    if server.gatherings.pop(g.gid, None) is not None:
        server.stats.gathering_removed(g, why)
    for pid in g.participants:
        if pid != g.owner:
            server.notify(pid, g.owner, NOTIFY_GONE, g.gid)
    log.debug("gathering %d removed: %s", g.gid, why)


def principal_gone(server, pid: int):
    """A session ended: its gatherings go, its participations end."""
    for g in list(server.gatherings.values()):
        if g.owner == pid:
            _remove(server, g, "owner disconnected")
        elif pid in g.participants:
            g.participants.remove(pid)
            server.notify(g.owner, pid, NOTIFY_LEAVE, g.gid, pid)


def _tunnelled(server, pid) -> bool:
    c = server.sessions.get(pid)
    return c is not None and c.vip is not None


def _same_pool(server, c, g) -> bool:
    """A tunnelled player (relay DLL, its station URL a virtual 10.254.x.y address) and one without the tunnel (an older
    DLL, online_relay = off, a relay that did not answer) cannot reach each other: the server announces only the
    virtual address of the first, and the second has no tunnel to send to it. They see and join only rooms of their
    own kind (the room's owner decides)."""
    return _tunnelled(server, c.pid) == _tunnelled(server, g.owner)


def _get(server, gid: int) -> Gathering:
    g = server.gatherings.get(gid)
    if g is None:
        raise RmcError(codec.ERR_INVALID_GID)
    return g


def _range(r):
    offset, size = r.u32(), r.u32()
    return offset, min(size, MAX_RESULTS)


def _listing(found, offset, size) -> bytes:
    page = found[offset:offset + size]
    return Writer().list(page, lambda w, g: g.put(w)).bytes()


@rmc("secure", 21, 1)
def register_gathering(server, c, r):
    name, body = r.anydata(1024)
    g = Gathering.parse(name, body)
    for old in [x for x in server.gatherings.values() if x.owner == c.pid]:
        _remove(server, old, "owner registered another")
    if len(server.gatherings) >= server.limits.max_gatherings:
        raise RmcError(codec.ERR_ACCESS_DENIED)
    g.gid, g.owner, g.host = server.next_gid, c.pid, c.pid
    server.next_gid += 1
    server.gatherings[g.gid] = g
    server.count("gatherings_registered")
    server.stats.gathering_registered(g)
    return Writer().u32(g.gid).bytes()


@rmc("secure", 21, 2)
def unregister_gathering(server, c, r):
    g = server.gatherings.get(r.u32())
    if g is None or g.owner != c.pid:
        return Writer().bool(False).bytes()
    _remove(server, g, "unregistered")
    return Writer().bool(True).bytes()


@rmc("secure", 21, 4)
def update_gathering(server, c, r):
    name, body = r.anydata(1024)
    new = Gathering.parse(name, body)
    g = server.gatherings.get(new.gid)
    if g is None or new.type != g.type:
        return Writer().bool(False).bytes()
    if g.owner != c.pid:
        # A joiner sends the gathering back unchanged right after its join (seen 2026-11-11): accepted, not applied.
        return Writer().bool(c.pid in g.participants).bytes()
    g.min, g.max, g.policy, g.policy_arg, g.flags, g.state = new.min, new.max, new.policy, new.policy_arg, new.flags, new.state
    g.description, g.tail = new.description, new.tail
    return Writer().bool(True).bytes()


@rmc("secure", 21, 11)
def participate(server, c, r):
    g = _get(server, r.u32())
    message = r.string(256)
    if c.pid not in g.participants and not _same_pool(server, c, g):
        server.count("participate_refused_tunnel")
        raise RmcError(codec.ERR_ACCESS_DENIED)
    if c.pid not in g.participants:
        if g.max and len(g.participants) >= g.max:
            raise RmcError(codec.ERR_SESSION_FULL)
        g.participants.append(c.pid)
        if c.pid != g.owner:
            g.joined = True
            server.notify(g.owner, c.pid, NOTIFY_JOIN, g.gid, c.pid, message)
    return Writer().bool(True).bytes()


@rmc("secure", 21, 12)
def cancel_participation(server, c, r):
    g = server.gatherings.get(r.u32())
    r.string(256)
    if g is None or c.pid not in g.participants:
        return Writer().bool(False).bytes()
    g.participants.remove(c.pid)
    if c.pid != g.owner:
        server.notify(g.owner, c.pid, NOTIFY_LEAVE, g.gid, c.pid)
    return Writer().bool(True).bytes()


@rmc("secure", 21, 16)
def get_participants_urls(server, c, r):
    """The stations of the participants who joined before the asker, then the asker's own, each URL with PID.

    The game's join code (0xA29D90-0xA29E36 in the image) walks this list, adds every URL to its probe list until it
    meets a URL whose PID is its own, and only then stops. It has no end-of-list check before that: a list without
    the asker's PID runs off the end into the runtime's invalid-iterator abort (0x7582F9), which is how the game
    process ended right after this call (exit code 13, no exception). So the asker's own stations must be in the
    answer, after the ones it is to probe; later joiners are left out (the game would not read them)."""
    g = _get(server, r.u32())
    order = sorted(g.participants, key=lambda p: p != g.host)    # joining order, the host first
    if c.pid in order:
        order = order[:order.index(c.pid)]
    urls = []
    for pid in order:
        s = server.sessions.get(pid)
        if s is not None:
            urls += _station_urls(s)
    own = _station_urls(c)
    urls = urls[:MAX_ANSWER_URLS - len(own)] + own
    return Writer().list(urls, lambda w, u: w.string(u)).bytes()


@rmc("secure", 21, 17)
def find_by_type(server, c, r):
    kind = r.string(64)
    offset, size = _range(r)
    return _listing([g for g in server.gatherings.values() if g.type == kind and _same_pool(server, c, g)], offset, size)


@rmc("secure", 21, 18)
def find_by_description(server, c, r):
    text = r.string(256)
    offset, size = _range(r)
    return _listing([g for g in server.gatherings.values() if g.description == text and _same_pool(server, c, g)],
                    offset, size)


@rmc("secure", 21, 21)
def find_by_single_id(server, c, r):
    g = _get(server, r.u32())
    w = Writer().bool(True)
    g.put(w)
    return w.bytes()


@rmc("secure", 21, 26)
def launch_session(server, c, r):
    g = server.gatherings.get(r.u32())
    url = r.string(256)
    if g is None or g.owner != c.pid:
        return Writer().bool(False).bytes()
    g.session_url = url
    return Writer().bool(True).bytes()


@rmc("secure", 21, 30)
def set_state(server, c, r):
    g = server.gatherings.get(r.u32())
    state = r.u32()
    if g is None or g.owner != c.pid:
        return Writer().bool(False).bytes()
    g.state = state
    return Writer().bool(True).bytes()


# Invitations are not kept yet: nobody has any, and inviting fails politely.
@rmc("secure", 21, 10)
def get_invitations_received(server, c, r):
    return Writer().u32(0).bytes()


@rmc("secure", 21, 5)
def invite(server, c, r):
    return Writer().bool(False).bytes()


@rmc("secure", 21, 6)
def accept_invitation(server, c, r):
    return Writer().bool(False).bytes()


@rmc("secure", 21, 7)
def decline_invitation(server, c, r):
    return Writer().bool(True).bytes()
