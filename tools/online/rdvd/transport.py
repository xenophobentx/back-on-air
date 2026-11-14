"""PRUDP v0 server side: connections, acknowledgements, ordering, resends, fragments, timeouts.

No sockets here: an Endpoint is fed datagrams (`datagram`) and time (`tick`) and writes through a callback, so the
whole server runs in tests without a network. One Endpoint per UDP port (authentication, secure).

Signatures [captured]: every packet a client sends after its SYN carries the connection signature we gave it in
the SYN acknowledgement; every packet we send carries the one the client gave in its CONNECT. Together with the
source address they are the only protection this protocol has against forged packets, so ours are random.
"""
import os
import struct

from . import codec
from .codec import Packet, SYN, CONNECT, DATA, DISCONNECT, PING, F_ACK, F_RELIABLE, F_NEED_ACK, F_HAS_SIZE, WireError

RESEND_AFTER = 1.5        # seconds before a reliable packet of ours is sent again
RESEND_TRIES = 8          # then the connection is dropped
HANDSHAKE_TIMEOUT = 5.0   # SYN without CONNECT (the game sends its CONNECT right after the SYN acknowledgement)
IDLE_PING = 10.0          # silence after which we ping the client, then again every PING_EVERY while it stays silent.
                          # Home routers often forget a UDP mapping after about 30 s without traffic; a client that
                          # answers a ping every 10 s keeps its mapping open through a race.
PING_EVERY = 10.0
IDLE_DROP = 180.0         # silence after which the connection is dropped. 75 s dropped a racing game on 2026-11-12:
                          # in a race the game makes no calls, and one of three games (Ubuntu, Proton) sent nothing
                          # for 75 s from the race's first minute while the other two stayed; one lost ping was all
                          # it got. A dead client still goes within 3 minutes.
IDLE_DROP_IN_GAME = 1200.0  # the same for a player in a room with another player (Server.in_game): a race with its
                          # loading and results makes no call at all, and a dropped host takes its room with it. A
                          # crashed player of such a room keeps its place up to 20 minutes.
AUTH_IDLE_DROP = 20.0     # the authentication port: a login takes a second, and the game leaves failed ones open
MAX_OUT_OF_ORDER = 32
MAX_MESSAGE = 64 * 1024   # reassembled message size


class Connection:
    def __init__(self, endpoint, addr, ident, now):
        self.endpoint, self.addr, self.id = endpoint, addr, ident
        self.client_sig = None            # set by CONNECT
        self.session = 0
        self.vport_ours, self.vport_theirs = 0x31, 0x3F
        self.has_size = 0                 # the client's HAS_SIZE habit, mirrored
        self.recv_next = 2                # SYN is 0, CONNECT is 1
        self.ahead = {}                   # seq -> Packet received early
        self.fragments = b""
        self.send_seq = 0
        self.unacked = {}                 # seq -> [datagram, last sent, tries]
        self.ping_seq = 0
        self.created = self.last_rx = now
        self.last_ping = None             # when we last pinged it during the current silence
        self.closed = False
        self.next_call = 1
        # set by the services
        self.pid = None                   # principal on the secure connection; name looked up on the auth one
        self.name = None
        self.cid = None                   # connection id handed out by Register
        self.urls = []                    # station URLs registered by the client
        self.public_url = None
        self.vip = None                   # virtual address, when the client's DLL tunnels its game traffic (relay.py)
        self.login_pid = None             # auth connection: the principal that got a Login ticket

    @property
    def connected(self):
        return self.client_sig is not None and not self.closed

    def _send(self, p: Packet):
        p.src, p.dst, p.session, p.sig = self.vport_ours, self.vport_theirs, self.session, self.client_sig or 0
        b = p.encode(self.endpoint.base)
        self.endpoint.send(b, self.addr)
        return b

    def send_message(self, plain: bytes):
        """One RMC message, reliably, in as many DATA packets as it needs (last fragment has id 0)."""
        if self.closed:
            return
        parts = [plain[i:i + codec.MAX_FRAGMENT] for i in range(0, len(plain), codec.MAX_FRAGMENT)] or [b""]
        for i, part in enumerate(parts):
            self.send_seq = (self.send_seq + 1) & 0xFFFF
            p = Packet(DATA, F_RELIABLE | F_NEED_ACK | self.has_size, seq=self.send_seq,
                       frag=0 if i == len(parts) - 1 else i + 1, payload=codec.wrap_payload(part))
            p.src, p.dst, p.session, p.sig = self.vport_ours, self.vport_theirs, self.session, self.client_sig or 0
            b = p.encode(self.endpoint.base)
            self.unacked[self.send_seq] = [b, self.endpoint.now, 1]     # before sending: the acknowledgement may be instant
            self.endpoint.send(b, self.addr)

    def call(self, protocol: int, method: int, params: bytes):
        """A call of ours on the client (notifications, NAT probe requests). Its answer, if any, is ignored."""
        cid = self.next_call
        self.next_call += 1
        self.send_message(codec.encode_request(protocol, cid, method, params))

    def close(self, why: str):
        if not self.closed:
            self.closed = True
            self.endpoint.forget(self, why)


class Endpoint:
    def __init__(self, server, kind: str, port: int, send):
        self.server, self.kind, self.port, self.send = server, kind, port, send
        self.base = codec.key_sum(server.access_key)
        self.conns = {}          # our connection signature -> Connection
        self.half_open = {}      # the same for those with no CONNECT yet, oldest first
        self.now = 0.0

    # ------------------------------------------------------------------------------------------------- receive
    def datagram(self, data: bytes, addr, now: float):
        self.now = now
        lim = self.server.limits
        if not lim.packets.allow(addr[0], now):
            self.server.count("dropped_rate")
            return
        try:
            packets = codec.decode_packets(data, self.base)
        except WireError as e:
            self.server.count("dropped_malformed")
            self.server.debug("%s: %s from %s", self.kind, e, addr[0])
            return
        for p in packets:
            try:
                self.packet(p, addr, now)
            except WireError as e:
                self.server.count("dropped_malformed")
                self.server.debug("%s: %s in %r", self.kind, e, p)

    def packet(self, p: Packet, addr, now: float):
        if p.type == SYN:
            if p.flags & F_ACK:
                return
            return self.syn(p, addr, now)
        c = self.conns.get(p.sig)
        if c is None or c.closed:
            self.server.count("dropped_unknown")
            return
        if c.addr != addr:
            # A router that forgot the mapping in a silence gives the client a new port. The signature is random and
            # never leaves the connection, so the same public address with it is the same client; another address is not.
            if not c.connected or c.addr[0] != addr[0]:
                self.server.count("dropped_unknown")
                return
            c.addr = addr
            self.server.count("port_changed")
        c.last_rx, c.last_ping = now, None
        if p.flags & F_ACK:
            if p.type == DATA:
                c.unacked.pop(p.seq, None)
            return
        if p.type == CONNECT:
            return self.connect(c, p)
        if not c.connected:
            return
        if p.type == DATA:
            return self.data(c, p)
        if p.type == PING:
            c._send(Packet(PING, F_ACK | (p.flags & F_HAS_SIZE), seq=p.seq))
        elif p.type == DISCONNECT:
            c._send(Packet(DISCONNECT, F_ACK | (p.flags & F_HAS_SIZE), seq=p.seq))
            c.close("client disconnected")

    def syn(self, p: Packet, addr, now: float):
        lim = self.server.limits
        if not lim.syn.allow(addr[0], now):
            self.server.count("dropped_rate")
            return
        same = [c for c in self.conns.values() if c.addr[0] == addr[0]]
        if len(same) >= lim.max_per_address or sum(1 for c in same if c.client_sig is None) >= lim.max_half_open_per_address:
            self.server.count("refused_full")
            return
        # Half-open connections cost an attacker one datagram each, from any forged address; the oldest of them goes
        # first, so that a flood cannot keep real players out. Established connections are never evicted.
        if self.half_open and (len(self.half_open) >= lim.max_half_open or len(self.conns) >= lim.max_connections):
            next(iter(self.half_open.values())).close("handshake evicted")
            self.server.count("half_open_evicted")
        if len(self.conns) >= lim.max_connections:
            self.server.count("refused_full")
            return
        ident = 0
        while ident == 0 or ident in self.conns:
            ident = struct.unpack("<I", os.urandom(4))[0]
        c = self.conns[ident] = self.half_open[ident] = Connection(self, addr, ident, now)
        c.vport_ours, c.vport_theirs = p.dst, p.src
        # The SYN acknowledgement: the client's own signature field echoed, our signature in the connection field.
        r = Packet(SYN, F_ACK | (p.flags & F_HAS_SIZE), src=p.dst, dst=p.src, session=p.session, sig=p.sig, seq=p.seq, connsig=ident)
        self.send(r.encode(self.base), addr)

    def connect(self, c: Connection, p: Packet):
        if c.client_sig is not None:      # a repeated CONNECT: acknowledge again, change nothing
            if p.connsig == c.client_sig:
                c._send(Packet(CONNECT, F_ACK | c.has_size, seq=p.seq, connsig=c.id, payload=getattr(c, "connect_reply", b"")))
            return
        reply = b""
        if self.kind == "secure":
            reply = self.server.secure_connect(c, codec.unwrap_payload(p.payload))   # raises WireError to refuse
        elif p.payload:
            raise WireError("CONNECT payload on the authentication port")
        c.client_sig, c.session, c.has_size = p.connsig, p.session, p.flags & F_HAS_SIZE
        self.half_open.pop(c.id, None)
        c.connect_reply = codec.wrap_payload(reply) if reply else b""
        c._send(Packet(CONNECT, F_ACK | c.has_size, seq=p.seq, connsig=c.id, payload=c.connect_reply))

    def data(self, c: Connection, p: Packet):
        ack = Packet(DATA, F_ACK | (p.flags & F_HAS_SIZE), seq=p.seq, frag=p.frag)
        if not p.flags & F_RELIABLE:
            return self.message(c, codec.unwrap_payload(p.payload))
        behind = (c.recv_next - p.seq) & 0xFFFF
        if 0 < behind < 0x8000:           # a repeat of something already handled: acknowledge again
            c._send(ack)
            return
        if p.seq != c.recv_next:
            if len(c.ahead) < MAX_OUT_OF_ORDER:
                c.ahead[p.seq] = p
                c._send(ack)
            return
        c._send(ack)
        while p is not None and not c.closed:
            c.recv_next = (c.recv_next + 1) & 0xFFFF
            part = codec.unwrap_payload(p.payload)
            if p.frag:
                c.fragments += part
                if len(c.fragments) > MAX_MESSAGE:
                    c.close("message too large")
                    return
            else:
                whole, c.fragments = c.fragments + part, b""
                self.message(c, whole)
            p = c.ahead.pop(c.recv_next, None)

    def message(self, c: Connection, plain: bytes):
        if not plain:
            return
        req = codec.decode_message(plain)
        if req is None:
            return                         # an answer to a call of ours
        if not self.server.limits.calls.allow((self.port, c.id), self.now):
            self.server.count("dropped_rate")
            c.send_message(codec.encode_error(req.protocol, req.call, codec.ERR_ACCESS_DENIED))
            return
        c.send_message(self.server.dispatch(c, req))

    # ---------------------------------------------------------------------------------------------------- time
    def tick(self, now: float):
        self.now = now
        for c in list(self.conns.values()):
            if c.client_sig is None:
                if now - c.created > HANDSHAKE_TIMEOUT:
                    c.close("handshake timeout")
                continue
            for seq, u in list(c.unacked.items()):
                if now - u[1] >= RESEND_AFTER:
                    if u[2] >= RESEND_TRIES:
                        c.close("no acknowledgement")
                        break
                    u[1], u[2] = now, u[2] + 1
                    self.send(u[0], c.addr)
            if c.closed:
                continue
            idle = now - c.last_rx
            if self.kind == "auth":
                if idle > AUTH_IDLE_DROP:
                    c.close("silent")
            elif idle > IDLE_DROP and (idle > IDLE_DROP_IN_GAME or not self.server.in_game(c.pid)):
                c.close("silent")
            elif idle > IDLE_PING and (c.last_ping is None or now - c.last_ping >= PING_EVERY):
                c.last_ping = now
                c.ping_seq = (c.ping_seq + 1) & 0xFFFF
                c._send(Packet(PING, F_NEED_ACK | c.has_size, seq=c.ping_seq))

    def forget(self, c: Connection, why: str):
        self.conns.pop(c.id, None)
        self.half_open.pop(c.id, None)
        self.server.connection_closed(c, why)
