"""SecureConnection (protocol 11) and NATTraversal (protocol 3): who is where.

Register tells the client the address its datagrams really come from (its public mapping; the Rendez-Vous
connection uses the game's own UDP socket, so this is also where peers can reach it) and gives it a connection id.
"""
from . import codec, relay
from .codec import Writer
from .server import rmc

MAX_URLS = 6


def _station_urls(c):
    """What other participants are told about this client: the public mapping first, then what it registered.

    Every URL carries the owner's PID and RVCID: the game's join code reads PID from each participant URL (it
    collects the stations to probe until it meets its own pid, 0xA29D90 in the image)."""
    out = []
    for u in ([c.public_url] if c.public_url else []) + list(c.urls):
        scheme, f = codec.parse_url(u)
        f["PID"], f["RVCID"] = c.pid, c.cid or 0
        u = codec.make_url(scheme, f)
        if u not in out:
            out.append(u)
    return out


def _register(server, c, r):
    urls = r.list(lambda x: x.string(256), MAX_URLS)
    c.urls = [u for u in urls if u.startswith("prudp:/")]
    if c.cid is None:
        c.cid = server.next_cid
        server.next_cid += 1
    port = server.relay.bind(c, c.urls)
    if port:
        # A tunnelled client: the only address anybody is told is the virtual one (the game registered it itself).
        c.public_url, c.urls = "prudp:/address=%s;port=%d;sid=15;type=3" % (relay.vip_text(c.vip), port), []
    else:
        c.public_url = "prudp:/address=%s;port=%d;sid=15;type=3" % (c.addr[0], c.addr[1])
    return Writer().u32(codec.OK).u32(c.cid).string(c.public_url).bytes()


@rmc("secure", 11, 1, guest=True)
def register(server, c, r):
    return _register(server, c, r)


@rmc("secure", 11, 4, guest=True)
def register_ex(server, c, r):
    return _register(server, c, r)   # the custom data that follows the URL list is not used


def _target(server, r):
    r.u32()                          # connection id of the target: the pid is enough here (one session per account)
    return server.sessions.get(r.u32())


@rmc("secure", 11, 2)
def request_connection_data(server, c, r):
    t = _target(server, r)
    w = Writer().bool(t is not None)
    urls = _station_urls(t) if t else []
    return w.list(urls, lambda x, u: x.string(u).u32(t.cid or 0)).bytes()


@rmc("secure", 11, 3)
def request_urls(server, c, r):
    t = _target(server, r)
    return Writer().bool(t is not None).list(_station_urls(t) if t else [], lambda x, u: x.string(u)).bytes()


def _probe(server, c, targets):
    """Ask each target station to send a probe to the requester (NATTraversal method 2 on the target's connection)."""
    mine = _station_urls(c)
    if not mine:
        return
    mine = mine[0]
    wanted = set()
    for u in targets:
        _scheme, f = codec.parse_url(u)
        wanted.add((f.get("address"), f.get("port")))
    for t in list(server.sessions.values()):
        if t is c:
            continue
        for u in _station_urls(t):
            _scheme, f = codec.parse_url(u)
            if (f.get("address"), f.get("port")) in wanted:
                t.call(3, 2, Writer().string(mine).bytes())
                server.count("probes_relayed")
                break


@rmc("secure", 3, 1)
def request_probe_initiation(server, c, r):
    _probe(server, c, r.list(lambda x: x.string(256), 16))
    return b""


@rmc("secure", 3, 3)
def request_probe_initiation_ext(server, c, r):
    targets = r.list(lambda x: x.string(256), 16)
    _probe(server, c, targets)
    return b""
