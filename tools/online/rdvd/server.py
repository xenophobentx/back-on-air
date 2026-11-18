"""The server object: two endpoints (authentication, secure), the call dispatcher, the shared state.

Sans-IO: `Server` is driven by `datagram()` and `tick()`; `__main__.py` puts it on two UDP sockets with asyncio.
Handlers live in the service modules and register themselves with `@rmc(kind, protocol, method)`.
"""
import logging
import struct
import time

from . import codec, kerberos
from .accounts import Accounts
from .codec import Reader, Writer, WireError
from .limits import Limits
from .relay import Relay
from .stats import Stats
from .transport import Endpoint

log = logging.getLogger("rdvd")

HANDLERS = {}     # (endpoint kind, protocol, method) -> (function, guest allowed, name)
PROTOCOL_NAMES = {3: "NATTraversal", 10: "Authentication", 11: "SecureConnection", 14: "Notification", 20: "Friends",
                  21: "MatchMaking", 23: "Messaging", 25: "AccountManagement", 26: "Competition", 27: "MessageDelivery",
                  50: "MatchMakingExt", 101: "RVLeaderboard", 102: "RVPlayer", 111: "RVValidation"}


def rmc(kind: str, protocol: int, method: int, guest: bool = False):
    def deco(fn):
        HANDLERS[(kind, protocol, method)] = (fn, guest, fn.__name__)
        return fn
    return deco


class RmcError(Exception):
    """Raised by a handler: the call is answered with an RMC error carrying this code."""

    def __init__(self, code: int):
        Exception.__init__(self, "0x%08X" % code)
        self.code = code


class Server:
    def __init__(self, db_path: str, secret: bytes, public_address: str, auth_port: int = 31120, secure_port: int = 31121,
                 access_key: str = "3wyEgW4x", send_auth=None, send_secure=None, send_relay=None, log_calls: bool = False,
                 notifications: bool = False, stats=None):
        self.access_key, self.secret, self.public_address = access_key, secret, public_address
        self.auth_port, self.secure_port = auth_port, secure_port
        self.log_calls, self.notifications = log_calls, notifications
        self.accounts = Accounts(db_path, secret)
        self.limits = Limits()
        self.auth = Endpoint(self, "auth", auth_port, send_auth or (lambda b, a: None))
        self.secure = Endpoint(self, "secure", secure_port, send_secure or (lambda b, a: None))
        self.relay = Relay(self, send_relay or (lambda b, a: None))
        self.guest_key = kerberos.principal_key(kerberos.GUEST_PID, kerberos.password_hash(kerberos.GUEST_PASSWORD))
        self.sessions = {}        # pid -> Connection on the secure endpoint (one per account)
        self.next_cid = 1
        self.gatherings = {}      # gid -> matchmaking.Gathering
        self.next_gid = 1
        self.counters = {}
        self.started = time.time()
        self.last_prune = 0.0
        self.stats = stats or Stats()
        self.stats.bind(self)

    # ------------------------------------------------------------------------------------------------ plumbing
    def count(self, what: str, n: int = 1):
        self.counters[what] = self.counters.get(what, 0) + n

    def debug(self, fmt, *args):
        log.debug(fmt, *args)

    def datagram(self, kind: str, data: bytes, addr, now: float):
        if kind == "relay":
            return self.relay.datagram(data, addr, now)
        (self.auth if kind == "auth" else self.secure).datagram(data, addr, now)

    def tick(self, now: float):
        self.auth.tick(now)
        self.secure.tick(now)
        if now - self.last_prune > 60:
            self.last_prune = now
            self.limits.prune(now)
            self.relay.tick(now)
        self.stats.tick(now)

    def secure_url(self) -> str:
        return "prudps:/address=%s;port=%d;CID=1;PID=%d;sid=1;stream=3;type=2" % (
            self.public_address, self.secure_port, kerberos.SERVER_PID)

    # ------------------------------------------------------------------------------------------------ dispatch
    def dispatch(self, c, req) -> bytes:
        kind = c.endpoint.kind
        h = HANDLERS.get((kind, req.protocol, req.method))
        label = "%s %s.%d" % (kind, PROTOCOL_NAMES.get(req.protocol, req.protocol), req.method)
        if self.log_calls:
            log.info("call %s pid=%s params=%s", label, c.pid, req.params.hex())
        if h is None:
            self.count("calls_unknown")
            log.info("unknown call %s (%d bytes) from pid %s", label, len(req.params), c.pid)
            return codec.encode_error(req.protocol, req.call, codec.ERR_NOT_IMPLEMENTED)
        fn, guest_ok, name = h
        c.last_call = c.last_rx           # the race-start heuristic (stats.py) watches the host's calls
        if kind == "secure" and (c.pid is None or (c.pid == kerberos.GUEST_PID and not guest_ok)):
            self.count("calls_denied")
            return codec.encode_error(req.protocol, req.call, codec.ERR_ACCESS_DENIED)
        try:
            body = fn(self, c, Reader(req.params))
        except RmcError as e:
            self.count("calls_error")
            if self.log_calls:
                log.info("  %s -> error %s", name, e)
            return codec.encode_error(req.protocol, req.call, e.code)
        except WireError as e:
            self.count("calls_malformed")
            log.info("malformed %s (%s): %s", label, name, e)
            return codec.encode_error(req.protocol, req.call, codec.ERR_INVALID_ARGUMENT)
        self.count("calls_ok")
        if self.log_calls:
            log.info("  %s -> %s", name, body.hex())
        return codec.encode_success(req.protocol, req.call, req.method, body)

    # ------------------------------------------------------------------------------------- the secure handshake
    def secure_connect(self, c, plain: bytes) -> bytes:
        """CONNECT payload of the secure port: Buffer internal ticket, Buffer check data. -> the acknowledgement's payload."""
        r = Reader(plain)
        ticket = r.buffer(256)
        check = r.buffer(64)
        pid, session_key = kerberos.open_internal_ticket(self.secret, ticket, time.time())
        cpid, _cid, value = kerberos.open_connect_check(session_key, check)
        if cpid != pid:
            raise WireError("check data names another principal")
        c.pid = pid
        if pid == kerberos.GUEST_PID:
            c.name = kerberos.GUEST_NAME
        else:
            c.name = self.accounts.name_of(pid)
            if c.name is None:
                raise WireError("ticket for a deleted account")
            old = self.sessions.get(pid)
            if old is not None and old is not c:
                old.close("logged in again elsewhere")
            self.sessions[pid] = c
            self.accounts.touch(pid)
            self.count("logins")
            self.stats.signin(pid, c.addr[0], self.accounts.created_day(pid))
        return Writer().buffer(struct.pack("<I", (value + 1) & 0xFFFFFFFF)).bytes()

    def connection_closed(self, c, why: str):
        log.debug("%s connection of pid %s closed: %s", c.endpoint.kind, c.pid, why)
        if c.endpoint.kind != "secure" or c.pid is None:
            return
        if c.vip is not None:
            self.relay.release(c.pid)
        if self.sessions.get(c.pid) is c:
            del self.sessions[c.pid]
            from . import matchmaking
            matchmaking.principal_gone(self, c.pid)

    def in_game(self, pid) -> bool:
        """The player owns or takes part in a room that has another player too (transport.IDLE_DROP_IN_GAME)."""
        if pid is None:
            return False
        for g in self.gatherings.values():
            members = set(g.participants)
            members.add(g.owner)
            if pid in members and len(members) > 1:
                return True
        return False

    def notify(self, pid: int, source: int, kind: int, p1: int = 0, p2: int = 0, text: str = ""):
        """Notification protocol (14), method 1, on the client's connection. Off unless enabled (layout unverified)."""
        c = self.sessions.get(pid)
        if c is None or not self.notifications:
            return
        c.call(14, 1, Writer().u32(source).u32(kind).u32(p1).u32(p2).string(text).bytes())

    def snapshot(self) -> dict:
        """The 10-minute log line. (Not `stats`: that name is the Stats object of stats.py, which shadowed this method
        and made the server stop with a TypeError after 600 s.)"""
        return {"uptime": int(time.time() - self.started), "online": len(self.sessions), "gatherings": len(self.gatherings),
                "accounts": self.accounts.count(), "counters": dict(self.counters)}


# The service modules register their handlers on import.
from . import auth, secure, matchmaking, misc  # noqa: E402,F401
