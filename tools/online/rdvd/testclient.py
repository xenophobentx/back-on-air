"""A minimal client that talks like the game does (same packets, same order of calls), for tests and experiments.

It is transport-agnostic: give it a `send(kind, datagram)` function and feed what comes back to `receive`. In the
unit tests both ends run in one process (tests/support.py); over real sockets see tools/online/rdv_client.py.
"""
import os
import struct

from . import codec, kerberos
from .codec import Packet, Reader, Writer, SYN, CONNECT, DATA, DISCONNECT, F_ACK, F_RELIABLE, F_NEED_ACK


class Link:
    """One PRUDP connection of the client (to the authentication or to the secure port)."""

    def __init__(self, client, kind):
        self.client, self.kind = client, kind
        self.base = codec.key_sum(client.access_key)
        self.server_sig = 0
        self.my_sig = struct.unpack("<I", os.urandom(4))[0] or 1
        self.session = os.urandom(1)[0]
        self.seq = 0
        self.recv_next = 1
        self.answers = {}      # call id -> (ok, method or error code, body)
        self.pushed = []       # requests the server sent us: (protocol, method, params)
        self.connect_reply = None
        self.syn_acked = False
        self.closed = False
        self.raw = []          # every packet received, for assertions

    def _send(self, p: Packet):
        p.src, p.dst = 0x3F, 0x31
        self.client.send(self.kind, p.encode(self.base))

    def receive(self, dgram: bytes):
        for p in codec.decode_packets(dgram, self.base):
            self.raw.append(p)
            if p.type == SYN and p.flags & F_ACK:
                self.server_sig, self.syn_acked = p.connsig, True
            elif p.type == CONNECT and p.flags & F_ACK:
                self.connect_reply = codec.unwrap_payload(p.payload)
            elif p.type == DATA and not p.flags & F_ACK:
                self._send(Packet(DATA, F_ACK, session=self.session, sig=self.server_sig, seq=p.seq, frag=p.frag))
                if p.seq != self.recv_next:
                    continue               # a resend of something we have, or a gap (the tests do not reorder)
                self.recv_next += 1
                self.partial = getattr(self, "partial", b"") + codec.unwrap_payload(p.payload)
                if p.frag:
                    continue
                msg, self.partial = self.partial, b""
                req = codec.decode_message(msg)
                if req is not None:
                    self.pushed.append((req.protocol, req.method, req.params))
                else:
                    _proto, ok, call, what, body = codec.decode_response(msg)
                    self.answers[call] = (ok, what, body)
            elif p.type == DISCONNECT and p.flags & F_ACK:
                self.closed = True

    def connect(self, payload: bytes = b""):
        self._send(Packet(SYN, F_NEED_ACK))
        if not self.syn_acked:
            raise AssertionError("no SYN acknowledgement")
        self.seq = 1
        self._send(Packet(CONNECT, F_RELIABLE | F_NEED_ACK, session=self.session, sig=self.server_sig, seq=1,
                          connsig=self.my_sig, payload=codec.wrap_payload(payload) if payload else b""))
        return self.connect_reply

    def call(self, protocol: int, method: int, params: bytes = b""):
        """-> (ok, body) for a success, (False, error code) for an RMC error, None when nothing came back."""
        cid = self.client.next_call
        self.client.next_call += 1
        self.seq += 1
        self._send(Packet(DATA, F_RELIABLE | F_NEED_ACK, session=self.session, sig=self.server_sig, seq=self.seq,
                          payload=codec.wrap_payload(codec.encode_request(protocol, cid, method, params))))
        a = self.answers.pop(cid, None)
        if a is None:
            return None
        return (True, a[2]) if a[0] else (False, a[1])

    def disconnect(self):
        self.seq += 1
        self._send(Packet(DISCONNECT, F_RELIABLE | F_NEED_ACK, session=self.session, sig=self.server_sig, seq=self.seq))


class Client:
    def __init__(self, send, access_key: str = "3wyEgW4x", local_url: str = "prudp:/address=player.example;port=9103;sid=15"):
        self.send, self.access_key, self.local_url = send, access_key, local_url
        self.next_call = 5                 # the game's first call id
        self.links = {"auth": None, "secure": None}
        self.pid = None
        self.cid = None
        self.public_url = None

    def receive(self, kind: str, dgram: bytes):
        if self.links[kind] is not None:
            self.links[kind].receive(dgram)

    def open(self, kind: str) -> Link:
        self.links[kind] = Link(self, kind)
        return self.links[kind]

    # ------------------------------------------------------------------------------------------- the game's flow
    def login(self, name: str, password: str):
        """Login, RequestTicket, secure connect, Register. -> "ok" or a short reason."""
        h = kerberos.password_hash(password)
        auth = self.open("auth")
        auth.connect()
        a = auth.call(10, 1, Writer().string(name).bytes())
        if a is None or not a[0]:
            return "login call failed: %r" % (a,)
        r = Reader(a[1])
        retval, pid, ticket = r.u32(), r.u32(), r.buffer()
        if retval != codec.OK:
            return "retval 0x%08X" % retval
        key = kerberos.principal_key(pid, h)
        try:
            kerberos.open_ticket(key, ticket)
        except codec.WireError:
            return "ticket does not open"
        url = r.string()
        a = auth.call(10, 3, Writer().u32(pid).u32(kerberos.SERVER_PID).bytes())
        if a is None or not a[0]:
            return "RequestTicket failed: %r" % (a,)
        r = Reader(a[1])
        if r.u32() != codec.OK:
            return "RequestTicket retval"
        session_key, _server_pid, internal = kerberos.open_ticket(key, r.buffer())
        sec = self.open("secure")
        value = struct.unpack("<I", os.urandom(4))[0]
        reply = sec.connect(Writer().buffer(internal).buffer(kerberos.make_connect_check(session_key, pid, 0, value)).bytes())
        if reply is None or Reader(reply).buffer() != struct.pack("<I", (value + 1) & 0xFFFFFFFF):
            return "secure connect refused"
        a = sec.call(11, 1, Writer().list([self.local_url], lambda w, u: w.string(u)).bytes())
        if a is None or not a[0]:
            return "Register failed: %r" % (a,)
        r = Reader(a[1])
        r.u32()
        self.cid, self.public_url = r.u32(), r.string()
        self.pid, self.secure_url = pid, url
        auth.disconnect()
        return "ok"

    def create_account(self, name: str, password: str):
        """The game's path: log in as guest, CreateAccount, leave. -> retval of CreateAccount, or a reason string."""
        why = self.login(kerberos.GUEST_NAME, kerberos.GUEST_PASSWORD)
        if why != "ok":
            return why
        sec = self.links["secure"]
        a = sec.call(25, 1, Writer().string(name).string(kerberos.password_hash(password).hex()).u32(0x20)
                     .string("racer@example.com").bytes())
        sec.disconnect()
        if a is None or not a[0]:
            return "CreateAccount failed: %r" % (a,)
        return Reader(a[1]).u32()

    def call(self, protocol: int, method: int, params: bytes = b""):
        return self.links["secure"].call(protocol, method, params)


def atv_session(description: str = "", max_players: int = 8, attributes=(0,) * 10, gid: int = 0) -> bytes:
    """Parameters of RegisterGathering / UpdateGathering for the game's session type."""
    body = Writer().u32(gid).u32(0).u32(0).u16(1).u16(max_players).u32(0).u32(0).u32(0).u32(0).string(description)
    for a in attributes:
        body.u32(a)
    return Writer().anydata("ATVGameSession", body.bytes()).bytes()
