"""The relay port: game datagrams between two players of one gathering, for pairs that cannot reach each other.

The game itself knows nothing of it. The DLL on each PC gives the game a virtual address (10.254.x.y) as its own
and carries every datagram addressed to another virtual address through this port (docs/plan-online.md, 4.2).

Wire format ("SR", version 1), all on one UDP port, everything little-endian:

    HELLO     "SR" 01 nonce[8]                        tunnel -> server   (also the keep-alive, every 20 s)
    WELCOME   "SR" 02 nonce[8] vip[4] key[16]         server -> tunnel   (to the address the HELLO came from)
    SEND      "SR" 03 dst vip[4] mac[8] payload       tunnel -> server   mac = HMAC-SHA256(key, "S" dst payload)[:8]
    DELIVER   "SR" 04 src vip[4] mac[8] payload       server -> tunnel   mac = HMAC-SHA256(receiver's key, "D" src payload)[:8]

Rules: a WELCOME (and with it the key) only goes to an address that asked, so a forged source learns nothing; a
SEND is forwarded only when its mac is right, the sender's and the receiver's virtual addresses belong to signed-in
sessions, and both take part in the same gathering; one datagram in, at most one out, never larger than it came
in plus nothing (no amplification); token buckets per registration. Anything else is dropped without an answer.

A registration is tied to a Rendez-Vous session when that session's Register call carries a station URL with the
virtual address, from the same source IP address as the HELLO.
"""
import hashlib
import hmac
import os
import struct

from .limits import Buckets

MAGIC = b"SR"
HELLO, WELCOME, SEND, DELIVER = 1, 2, 3, 4
NET = (10, 254)
REGISTRATION_LIFETIME = 90.0
MAX_PAYLOAD = 1400
MAX_REGISTRATIONS = 1024


def vip_text(vip: bytes) -> str:
    return "%d.%d.%d.%d" % tuple(vip)


def vip_bytes(text: str):
    try:
        parts = [int(x) for x in text.split(".")]
    except ValueError:
        return None
    if len(parts) != 4 or parts[0] != NET[0] or parts[1] != NET[1] or not all(0 <= x < 256 for x in parts):
        return None
    return bytes(parts)


def mac(key: bytes, tag: bytes, vip: bytes, payload: bytes) -> bytes:
    return hmac.new(key, tag + vip + payload, hashlib.sha256).digest()[:8]


class Registration:
    __slots__ = ("addr", "vip", "key", "seen", "pid")

    def __init__(self, addr, vip, key, now):
        self.addr, self.vip, self.key, self.seen, self.pid = addr, vip, key, now, None


class Relay:
    def __init__(self, server, send):
        self.server, self.send = server, send
        self.by_addr = {}        # (ip, port) -> Registration
        self.by_vip = {}         # vip bytes -> Registration
        self.hello = Buckets(0.5, 10.0)             # per source IP address
        self.traffic = Buckets(300.0, 600.0)        # datagrams per registration per second
        self.volume = Buckets(100000.0, 200000.0)   # bytes per registration per second

    # ----------------------------------------------------------------------------------------------- datagrams
    def datagram(self, data: bytes, addr, now: float):
        if len(data) < 3 or data[:2] != MAGIC:
            return self.server.count("relay_dropped")
        kind = data[2]
        if kind == HELLO and len(data) == 11:
            return self._hello(data[3:11], addr, now)
        if kind == SEND and 15 <= len(data) <= 15 + MAX_PAYLOAD:
            return self._forward(data, addr, now)
        self.server.count("relay_dropped")

    def _hello(self, nonce: bytes, addr, now: float):
        r = self.by_addr.get(addr)
        if r is None:
            if not self.hello.allow(addr[0], now) or len(self.by_addr) >= MAX_REGISTRATIONS:
                return self.server.count("relay_dropped")
            vip = None
            for _ in range(64):
                cand = bytes(NET) + os.urandom(2)
                if cand[3] not in (0, 255) and cand not in self.by_vip:
                    vip = cand
                    break
            if vip is None:
                return self.server.count("relay_dropped")
            r = Registration(addr, vip, os.urandom(16), now)
            self.by_addr[addr] = self.by_vip[vip] = r
            self.server.count("relay_registrations")
        r.seen = now
        self.send(MAGIC + bytes([WELCOME]) + nonce + r.vip + r.key, addr)

    def _forward(self, data: bytes, addr, now: float):
        src = self.by_addr.get(addr)
        if src is None:
            return self.server.count("relay_dropped")
        dst_vip, tag, payload = data[3:7], data[7:15], data[15:]
        if not hmac.compare_digest(tag, mac(src.key, b"S", dst_vip, payload)):
            return self.server.count("relay_dropped")
        src.seen = now
        dst = self.by_vip.get(dst_vip)
        if dst is None or src.pid is None or dst.pid is None or not self._share_gathering(src.pid, dst.pid):
            return self.server.count("relay_refused")
        if not self.traffic.allow(src.vip, now) or not self.volume.allow(src.vip, now, len(payload)):
            return self.server.count("relay_limited")
        self.send(MAGIC + bytes([DELIVER]) + src.vip + mac(dst.key, b"D", src.vip, payload) + payload, dst.addr)
        self.server.count("relay_datagrams")
        self.server.count("relay_bytes", len(payload))

    def _share_gathering(self, a: int, b: int) -> bool:
        for g in self.server.gatherings.values():
            if a in g.participants and b in g.participants:
                return True
        return False

    # ---------------------------------------------------------------------------------------- session binding
    def bind(self, c, urls) -> bool:
        """Called by Register: if one of the client's URLs names a virtual address registered from the same source
        IP, the session is tied to that registration. -> True when the session is tunnelled."""
        for u in urls:
            for part in u.split(";"):
                if part.startswith("address=") or "/address=" in part:
                    vip = vip_bytes(part.split("address=", 1)[1])
                    r = self.by_vip.get(vip) if vip else None
                    if r is not None and r.addr[0] == c.addr[0] and r.pid in (None, c.pid):
                        r.pid = c.pid
                        c.vip = r.vip
                        return True
        return False

    def release(self, pid: int):
        for r in self.by_addr.values():
            if r.pid == pid:
                r.pid = None

    def tick(self, now: float):
        for addr, r in list(self.by_addr.items()):
            if now - r.seen > REGISTRATION_LIFETIME:
                del self.by_addr[addr]
                self.by_vip.pop(r.vip, None)
        self.hello.prune(now)
        self.traffic.prune(now)
        self.volume.prune(now)
