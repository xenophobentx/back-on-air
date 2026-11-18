"""The relay port: game datagrams between two players of one gathering, for pairs that cannot reach each other.

The game itself knows nothing of it. The DLL on each PC (src/tunnel.cpp) gives the game a virtual address
(10.254.x.y) as its own and carries every datagram addressed to another virtual address itself: directly when a
direct path works both ways, through this port when not (Takedawn's relay model; docs/plan-online.md 4.2, SSPC-41).

Wire format "SR" version 2 (src/tunnel_core.h has the same table), one UDP port, ports and lengths big-endian:

    HELLO    "SR" 2 1 nonce[8] vip[4] lan_ip[4] lan_port[2] proof[8] flags[1] pad   48 bytes, tunnel -> server, every 20 s
             flags bit 0: relay only (online_relay = always): no PEERINFO about it to anybody
    WELCOME  "SR" 2 2 nonce[8] vip[4] status[1] key[16] pub_ip[4] pub_port[2]   39 bytes, to the HELLO's source only
    SEND     "SR" 2 3 dst vip[4] sport[2] dport[2] mac[8] payload   mac = HMAC-SHA256(sender key, bytes 0..11 + payload)[:8]
    DELIVER  "SR" 2 4 src vip[4] sport[2] dport[2] mac[8] payload   re-signed with the receiver's key, same size
    PEER     "SR" 2 5 vip[4] nonce[4] mac[8] pad                    52 bytes: "where is this co-player"
    PEERINFO "SR" 2 6 vip[4] nonce[4] pub_ip[4] pub_port[2] lan_ip[4] lan_port[2] pair_key[16] mac[8]   48 bytes

The virtual address is the DLL's own choice (it must exist before the game asks for its own address, which can be
at start-up); the HELLO claims it. A free address is granted with a new random key. A taken one is granted again only
to its own registration: the same source address, or a new source address that proves the old key (proof = HMAC of
bytes 0..21 under it: a NAT that changed the tunnel's port). Otherwise WELCOME status 1 and no key.

Rules (Takedawn's aries_server.c relay): nothing answers a datagram that fails a check, and no answer is larger than
its request; a SEND is forwarded only when its mac is right and sender and receiver take part in one gathering (or did
so within the last 30 minutes: the race goes on when a lobby connection drops, Takedawn's add-only members); token
buckets per registration (400 datagrams/s, 64 KiB/s, a burst of one second); registrations expire after 90 s without
a HELLO or a valid SEND. A registration is tied to a Rendez-Vous session when that session's Register carries a station
URL with the virtual address, from the same source IP address as the HELLO. No address is ever logged.
"""
import hashlib
import hmac
import ipaddress
import logging
import os
import socket
import struct

from .limits import Buckets

log = logging.getLogger("rdvd.relay")

MAGIC = b"SR\x02"
HELLO, WELCOME, SEND, DELIVER, PEER, PEERINFO = 1, 2, 3, 4, 5, 6
HELLO_LEN, WELCOME_LEN, PEER_LEN, PEERINFO_LEN, HDR = 48, 39, 52, 48, 20
NET = (10, 254)
REGISTRATION_LIFETIME = 90.0
PAIR_TTL = 1800.0            # Takedawn RL_GAME_TTL: a pair stays allowed 30 min after its gathering
MAX_PAYLOAD = 1400
MAX_REGISTRATIONS = 1024
MAX_PER_IP = 8               # registrations from one source IP address (a household behind one router; not one host filling the table)
IDLE_EVICT = 60.0            # a full table frees the oldest registration without a Rendez-Vous session seen this long ago
HELLO_RELAY_ONLY = 1         # HELLO byte 30: never hand this player's endpoints to a co-player (online_relay = always)
RATE_PPS, RATE_BPS = 400.0, 65536.0   # Takedawn RL_PPS / RL_BPS, a burst of one second

DROP_REASONS = ("bad", "tag", "member", "nodest", "rate", "full", "taken")


def vip_text(vip: bytes) -> str:
    return "%d.%d.%d.%d" % tuple(vip)


def vip_bytes(text: str):
    try:
        parts = [int(x) for x in text.split(".")]
    except ValueError:
        return None
    if len(parts) != 4 or parts[0] != NET[0] or parts[1] != NET[1] or not all(0 <= x < 256 for x in parts):
        return None
    if parts[3] in (0, 255):
        return None
    return bytes(parts)


def mac(key: bytes, *parts: bytes) -> bytes:
    return hmac.new(key, b"".join(parts), hashlib.sha256).digest()[:8]


def ip4(host: str) -> bytes:
    """The 4 bytes of a dotted IPv4 address; zeros for anything else (the tests use .test host names)."""
    try:
        return socket.inet_aton(host)
    except (OSError, TypeError):
        return bytes(4)


def lan_ok(ip: bytes) -> bool:
    """A claimed LAN address is passed on only when it is a private one (Takedawn believes private addresses only)."""
    try:
        a = ipaddress.IPv4Address(ip)
    except ValueError:
        return False
    return a.is_private and not a.is_loopback and not a.is_link_local and not a.is_unspecified and ip[:2] != bytes(NET)


class Registration:
    __slots__ = ("addr", "vip", "key", "seen", "pid", "lan", "private", "born")

    def __init__(self, addr, vip, key, now):
        self.addr, self.vip, self.key, self.seen, self.pid = addr, vip, key, now, None
        self.lan = bytes(6)
        self.private = False
        self.born = now


class Relay:
    def __init__(self, server, send):
        self.server, self.send = server, send
        self.by_addr = {}        # (ip, port) -> Registration
        self.by_vip = {}         # vip bytes -> Registration
        self.pairs = {}          # frozenset({vip, vip}) -> (last time allowed, gathering id)
        self.relayed_games = set()
        self.secret = os.urandom(32)                 # pair keys: HMAC(secret, "P" + both vips), per server run
        self.hello = Buckets(0.5, 10.0)              # HELLOs from one source IP: a keep-alive every 20 s, bursts allowed
        self.packets = Buckets(RATE_PPS, RATE_PPS)
        self.volume = Buckets(RATE_BPS, RATE_BPS)

    def drop(self, why: str):
        self.server.count("relay_dropped")
        self.server.count("relay_drop_" + why)

    # ----------------------------------------------------------------------------------------------- datagrams
    def datagram(self, data: bytes, addr, now: float):
        if len(data) < 4 or data[:3] != MAGIC:
            return self.drop("bad")
        kind = data[3]
        if kind == HELLO and len(data) == HELLO_LEN:
            return self._hello(data, addr, now)
        if kind == SEND and HDR < len(data) <= HDR + MAX_PAYLOAD:
            return self._forward(data, addr, now)
        if kind == PEER and len(data) == PEER_LEN:
            return self._peer(data, addr, now)
        self.drop("bad")

    def _hello(self, data: bytes, addr, now: float):
        nonce, vip, lan, proof = data[4:12], data[12:16], data[16:22], data[22:30]
        if vip_bytes(vip_text(vip)) is None:
            return self.drop("bad")
        r = self.by_vip.get(vip)
        if r is not None and r.addr != addr:
            if proof == bytes(8) or not hmac.compare_digest(proof, mac(r.key, data[:22])):
                self.drop("taken")      # somebody else's address: refused, the asker learns nothing else
                return self.send(MAGIC + bytes([WELCOME]) + nonce + vip + b"\x01" + bytes(22), addr)
            self.by_addr.pop(r.addr, None)   # the same tunnel behind a NAT that changed its port
            r.addr = addr
            self.by_addr[addr] = r
        if r is None:
            old = self.by_addr.get(addr)
            if old is not None:             # the same tunnel with a new address (a restart of the game)
                self.by_vip.pop(old.vip, None)
                del self.by_addr[addr]
            if not self.hello.allow(addr[0], now):
                return self.drop("rate")
            if sum(1 for a in self.by_addr if a[0] == addr[0]) >= MAX_PER_IP:
                return self.drop("per_ip")
            if len(self.by_vip) >= MAX_REGISTRATIONS and not self._evict(now):
                return self.drop("full")
            r = Registration(addr, vip, os.urandom(16), now)
            self.by_addr[addr] = self.by_vip[vip] = r
            for pair in [p for p in self.pairs if vip in p]:   # an address used before: its old pairs are not inherited
                del self.pairs[pair]
            self.server.count("relay_registrations")
        r.seen = now
        r.private = bool(data[30] & HELLO_RELAY_ONLY)
        r.lan = lan if lan_ok(lan[:4]) and not r.private else bytes(6)
        self.send(MAGIC + bytes([WELCOME]) + nonce + vip + b"\x00" + r.key + ip4(addr[0]) + struct.pack(">H", addr[1]),
                  addr)

    def _evict(self, now: float) -> bool:
        """A full table: the oldest registration that never got (or lost) its Rendez-Vous session for IDLE_EVICT goes."""
        idle = [r for r in self.by_addr.values() if r.pid is None and now - r.born >= IDLE_EVICT]
        if not idle:
            return False
        r = min(idle, key=lambda x: x.born)
        del self.by_addr[r.addr]
        self.by_vip.pop(r.vip, None)
        self.server.count("relay_evicted")
        return True

    def _sender(self, data: bytes, addr, signed_len: int, payload: bytes = b""):
        src = self.by_addr.get(addr)
        if src is None:
            self.drop("member")
            return None
        if not hmac.compare_digest(data[signed_len:signed_len + 8], mac(src.key, data[:signed_len], payload)):
            self.drop("tag")
            return None
        return src

    def _forward(self, data: bytes, addr, now: float):
        payload = data[HDR:]
        src = self._sender(data, addr, 12, payload)
        if src is None:
            return
        src.seen = now
        dst = self.by_vip.get(data[4:8])
        if dst is None or dst is src:
            return self.drop("nodest")
        gid = self._allowed(src, dst, now)
        if gid is None:
            return self.drop("member")
        if not self.packets.allow(src.vip, now) or not self.volume.allow(src.vip, now, len(payload)):
            return self.drop("rate")
        head = MAGIC + bytes([DELIVER]) + src.vip + data[8:12]
        self.send(head + mac(dst.key, head, payload) + payload, dst.addr)
        self.server.count("relay_datagrams")
        self.server.count("relay_bytes", len(payload))
        if gid not in self.relayed_games:
            self.relayed_games.add(gid)
            self.server.count("relay_games")
            log.info("relay: gathering %d relayed", gid)

    def _peer(self, data: bytes, addr, now: float):
        src = self._sender(data, addr, 12)
        if src is None:
            return
        src.seen = now
        dst = self.by_vip.get(data[4:8])
        if dst is None or dst is src:
            return self.drop("nodest")
        if self._allowed(src, dst, now) is None:
            return self.drop("member")
        if dst.private:
            return self.drop("private")     # online_relay = always: its addresses stay with the server
        if not self.packets.allow(src.vip, now):
            return self.drop("rate")
        a, b = sorted((src.vip, dst.vip))
        pair_key = hmac.new(self.secret, b"P" + a + b, hashlib.sha256).digest()[:16]
        body = (MAGIC + bytes([PEERINFO]) + dst.vip + data[8:12] + ip4(dst.addr[0]) + struct.pack(">H", dst.addr[1]) +
                dst.lan + pair_key)
        self.send(body + mac(src.key, body), addr)
        self.server.count("relay_peer_answers")

    def _allowed(self, a: Registration, b: Registration, now: float):
        """-> the gathering id that lets a and b talk, or None. Pairs are remembered for PAIR_TTL after the last use."""
        pair = frozenset((a.vip, b.vip))
        if a.pid is not None and b.pid is not None:
            for gid, g in self.server.gatherings.items():
                if a.pid in g.participants and b.pid in g.participants:
                    self.pairs[pair] = (now, gid)
                    return gid
        got = self.pairs.get(pair)
        if got is not None and now - got[0] < PAIR_TTL:
            self.pairs[pair] = (now, got[1])
            return got[1]
        return None

    # ---------------------------------------------------------------------------------------- session binding
    def bind(self, c, urls):
        """Called by Register: if one of the client's URLs names a virtual address registered from the same source
        IP, the session is tied to that registration. -> the game port of that URL when the session is tunnelled."""
        for u in urls:
            fields = dict(p.split("=", 1) for p in u.split("/", 1)[-1].split(";") if "=" in p)
            vip = vip_bytes(fields.get("address", ""))
            r = self.by_vip.get(vip) if vip else None
            if r is not None and r.addr[0] == c.addr[0] and r.pid in (None, c.pid):
                r.pid = c.pid
                c.vip = r.vip
                try:
                    return int(fields.get("port", "9103"))
                except ValueError:
                    return 9103
        return None

    def release(self, pid: int):
        """A session ended. Its pairs stay allowed for PAIR_TTL (the race outlives the lobby connection)."""
        for r in self.by_addr.values():
            if r.pid == pid:
                r.pid = None

    def tick(self, now: float):
        for addr, r in list(self.by_addr.items()):
            if now - r.seen > REGISTRATION_LIFETIME:
                del self.by_addr[addr]
                self.by_vip.pop(r.vip, None)
        for pair, (t, _gid) in list(self.pairs.items()):
            if now - t > PAIR_TTL:
                del self.pairs[pair]
        live = {gid for _t, gid in self.pairs.values()} | set(self.server.gatherings)
        self.relayed_games &= live
        self.hello.prune(now)
        self.packets.prune(now)
        self.volume.prune(now)
