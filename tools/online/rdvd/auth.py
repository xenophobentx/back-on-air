"""Authentication (protocol 10, on the authentication port) and AccountManagement (protocol 25, secure port).

Captured behaviour this follows (docs/plan-online.md, 2.4):
* a failed Login is a normal answer whose retval is the error; only then does the game offer to create the account;
* after a successful Login the client asks RequestTicket(its pid, 2) and uses that ticket for the secure server;
* CreateAccount arrives on a guest's secure connection with the key as 32 hex digits (MD5 x 65000 of the password).
"""
import logging
import re

from . import codec, kerberos
from .accounts import auto_tag, valid_name
from .codec import Writer
from .server import rmc, RmcError

log = logging.getLogger("rdvd")
HEX32 = re.compile(r"^[0-9a-fA-F]{32}$")


def _login_answer(retval: int, pid: int = 0, ticket: bytes = b"", url: str = "prudp:/") -> bytes:
    # u32 retval, u32 pid, Buffer ticket, RVConnectionData{StationURL, List<u8>, StationURL}, String message
    return Writer().u32(retval).u32(pid).buffer(ticket).string(url).u32(0).string("").string("").bytes()


def _principal(server, name: str):
    """-> (pid, key K) or an error code."""
    if name == kerberos.GUEST_NAME:
        return kerberos.GUEST_PID, server.guest_key
    acc = server.accounts.lookup(name)
    if acc is None:
        return codec.ERR_INVALID_USERNAME
    pid, _stored, h, disabled = acc
    if disabled:
        return codec.ERR_ACCOUNT_DISABLED
    return pid, kerberos.principal_key(pid, h)


@rmc("auth", 10, 1)
def login(server, c, r):
    name = r.string(64)
    now = c.endpoint.now
    # Tickets per name are limited because a ticket lets its holder test passwords offline; the guest's is public.
    # The per-name bucket is per source address too: anybody else draining it does not lock the name's owner out.
    if not server.limits.login.allow(c.addr[0], now) or (
            name != kerberos.GUEST_NAME and not server.limits.login_name.allow((name.lower(), c.addr[0]), now)):
        server.count("login_limited")
        raise RmcError(codec.ERR_ACCESS_DENIED)
    who = _principal(server, name)
    if isinstance(who, int):
        server.count("login_unknown")
        return _login_answer(who)
    pid, key = who
    c.login_pid, c.login_key = pid, key
    return _login_answer(codec.OK, pid, kerberos.make_ticket(server.secret, pid, key), server.secure_url())


@rmc("auth", 10, 2)
def login_ex(server, c, r):
    return login(server, c, r)       # the extra data (an AnyDataHolder) is not used by this title's flow


@rmc("auth", 10, 3)
def request_ticket(server, c, r):
    source, target = r.u32(), r.u32()
    if c.login_pid is None or source != c.login_pid or target != kerberos.SERVER_PID:
        raise RmcError(codec.ERR_ACCESS_DENIED)
    return Writer().u32(codec.OK).buffer(kerberos.make_ticket(server.secret, source, c.login_key)).bytes()


@rmc("secure", 25, 1, guest=True)
def create_account(server, c, r):
    """CreateAccount(String name, String key, u32 groups, String email) -> u32 retval."""
    name, key = r.string(64), r.string(128)
    r.u32()
    r.string(256)                     # the client's constant dummy address: never stored
    now = c.endpoint.now
    if c.pid != kerberos.GUEST_PID:
        raise RmcError(codec.ERR_ACCESS_DENIED)
    if not valid_name(name) or not HEX32.match(key):
        return Writer().u32(codec.ERR_INVALID_USERNAME).bytes()
    tag = auto_tag(name)
    if tag:
        # An automatic sign-in whose Steam name changed: the same key under the same tag is the same player. The
        # account keeps its pid and is renamed instead of a second account being made.
        old = server.accounts.find_by_key(bytes.fromhex(key), tag)
        if old is not None:
            if server.accounts.name_of(old) == name or server.accounts.rename(old, name):
                server.count("accounts_renamed")
                log.info("account renamed (pid %d)", old)
                return Writer().u32(codec.OK).bytes()
            return Writer().u32(codec.ERR_USERNAME_EXISTS).bytes()
    if server.accounts.lookup(name) is not None:
        return Writer().u32(codec.ERR_USERNAME_EXISTS).bytes()     # before the limits: a taken name costs no token
    if not server.limits.create.allow(c.addr[0], now) or not server.limits.create_global.allow("all", now):
        server.count("create_limited")
        return Writer().u32(codec.ERR_ACCESS_DENIED).bytes()
    pid = server.accounts.create(name, bytes.fromhex(key))
    if pid is None:
        return Writer().u32(codec.ERR_USERNAME_EXISTS).bytes()
    server.count("accounts_created")
    log.info("account created (pid %d)", pid)
    return Writer().u32(codec.OK).bytes()


@rmc("secure", 25, 4)
def change_password(server, c, r):
    """ChangePassword(String new key) -> bool. The key has the same 32-hex-digit form as at creation."""
    key = r.string(128)
    if not HEX32.match(key):
        return Writer().bool(False).bytes()
    server.accounts.set_key(c.pid, bytes.fromhex(key))
    return Writer().bool(True).bytes()
