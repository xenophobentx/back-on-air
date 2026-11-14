#!/usr/bin/env python3
"""rdv_stub.py: capture stub for Split/Second PC's Quazal Rendez-Vous backend (measurement only, not a server).

Listens on UDP (default 31120, the port the game dials), logs every datagram as hex, decodes it as Quazal PRUDP v0
(one-byte type/flags), checks the checksum against the access key, and answers just enough (SYN, CONNECT, PING,
DISCONNECT acknowledgements, DATA acknowledgements) for the client to send its first Rendez-Vous call (the
Authentication protocol's Login). RMC requests are decrypted (RC4, key "CD&ML"), inflated if compressed, and printed
with protocol, method and a guess at the strings they hold.

With --answer the stub goes one step further, to see what the client does next (still not a server: nothing is
stored, one fixed session key): Login/LoginEx of a name given with --account NAME:PASSWORD (and of "guest") gets a
Kerberos ticket made from that password (or NAME:md5x65000:<32 hex digits>, the hashed form the game sends), any other name gets RendezVous::InvalidUsername (--unknown retval: as the
retval of a normal answer, which is what makes the game offer to create the account); RequestTicket is answered; a
create-account call is accepted and its name and password are kept in memory for the rest of the run; the CONNECT
of the secure connection is checked and answered; SecureConnection Register gets a connection id; every other call
is logged and answered with Core::NotImplemented.

Standard library only.  Usage:
    python3 rdv_stub.py [--bind ADDRESS] [--port 31120] [--key 3wyEgW4x] [--log capture.log]
                        [--answer [--account NAME:PASSWORD ...] [--secure-port N] [--public-ip HOST]]
    python3 rdv_stub.py --verify <hex datagram> [--key ...]     # checksum check of one captured datagram, offline

Packet layouts and constants: public documentation of the protocol (Kinnay's NintendoClients wiki, "PRUDP Protocol",
"RMC Protocol", "Kerberos Authentication") and this project's own captures; no code from other projects is used.
"""
import argparse
import hashlib
import hmac
import select
import socket
import struct
import sys
import time
import zlib

TYPES = {0: "SYN", 1: "CONNECT", 2: "DATA", 3: "DISCONNECT", 4: "PING", 5: "NATPING/USER"}
F_ACK, F_RELIABLE, F_NEED_ACK, F_HAS_SIZE = 1, 2, 4, 8
PROTOCOLS = {1: "RemoteLogDevice", 3: "NATTraversal", 10: "Authentication", 11: "SecureConnection", 14: "Notification",
             20: "Friends", 21: "MatchMaking", 23: "Messaging", 24: "PersistentStore", 25: "AccountManagement",
             26: "Competition", 27: "MessageDelivery", 50: "MatchMakingExt"}
AUTH_METHODS = {1: "Login", 2: "LoginEx", 3: "RequestTicket", 4: "GetPID", 5: "GetName", 6: "LoginWithContext"}
GUEST_PASSWORD = "h7fyctiuucf"   # in the game image, beside JobCreateAccount::LoginGuest
SESSION_KEY = bytes(range(0x10, 0x20))
SERVER_PID = 2                   # "Quazal Rendez-Vous", the principal of the secure server
OK = 0x00010001                  # Core::NoError as a %retval%


def rc4(key: bytes, data: bytes) -> bytes:
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 255
        s[i], s[j] = s[j], s[i]
    out = bytearray()
    i = j = 0
    for b in data:
        i = (i + 1) & 255
        j = (j + s[i]) & 255
        s[i], s[j] = s[j], s[i]
        out.append(b ^ s[(s[i] + s[j]) & 255])
    return bytes(out)


def derive_key(pid: int, password: str) -> bytes:
    """Kerberos key of a principal: MD5 applied 65000 + pid % 1024 times to the password.

    "md5x65000:<hex>" is the form the game's create-account call carries (the first 65000 rounds already done by the
    client, as 32 hex digits): only the pid % 1024 rounds are left to do."""
    if password.startswith("md5x65000:"):
        k, rounds = bytes.fromhex(password[10:]), pid % 1024
    else:
        k, rounds = password.encode(), 65000 + pid % 1024
    for _ in range(rounds):
        k = hashlib.md5(k).digest()
    return k


def qstr(s: str) -> bytes:
    b = s.encode() + b"\0"
    return struct.pack("<H", len(b)) + b


def qbuf(b: bytes) -> bytes:
    return struct.pack("<I", len(b)) + b


def checksum1(data: bytes, base: int) -> int:
    """One-byte checksum: byte sum of the little-endian u32 word sum, plus the trailing bytes, plus the key sum."""
    n = len(data) & ~3
    words = struct.unpack_from("<%dI" % (n // 4), data)
    tmp = sum(words) & 0xFFFFFFFF
    return (base + sum(data[n:]) + sum(struct.pack("<I", tmp))) & 0xFF


def checksum4(data: bytes, base: int) -> int:
    """Four-byte checksum of the original Rendez-Vous library: key sum + u32 word sum of the zero-padded packet."""
    data = data + b"\0" * (-len(data) % 4)
    return (base + sum(struct.unpack("<%dI" % (len(data) // 4), data))) & 0xFFFFFFFF


def key_sum(key: str) -> int:
    return sum(key.encode("ascii")) & 0xFF


def checksum_report(dgram: bytes, keys) -> str:
    """Which checksum reading fits this datagram (assuming it holds exactly one packet)."""
    out = []
    for name, base in [("no key", 0)] + [("key '%s' (sum 0x%02X)" % (k, key_sum(k)), key_sum(k)) for k in keys]:
        if len(dgram) >= 2 and checksum1(dgram[:-1], base) == dgram[-1]:
            out.append("1-byte checksum 0x%02X matches with %s" % (dgram[-1], name))
        if len(dgram) >= 5 and checksum4(dgram[:-4], base) == struct.unpack("<I", dgram[-4:])[0]:
            out.append("4-byte checksum 0x%08X matches with %s" % (struct.unpack("<I", dgram[-4:])[0], name))
    return "; ".join(out) if out else "NO checksum reading matches (1 or 4 bytes, with or without the keys)"


class Packet:
    def __init__(self):
        self.src = self.dst = self.type = self.flags = self.session = 0
        self.sig = self.seq = self.connsig = self.frag = 0
        self.payload = b""

    def describe(self) -> str:
        fl = "|".join(n for b, n in ((1, "ACK"), (2, "RELIABLE"), (4, "NEED_ACK"), (8, "HAS_SIZE"), (16, "0x10")) if self.flags & b)
        s = "%s[%s] src=%X:%d dst=%X:%d session=%d sig=%08X seq=%d" % (
            TYPES.get(self.type, str(self.type)), fl, self.src >> 4, self.src & 15, self.dst >> 4, self.dst & 15,
            self.session, self.sig, self.seq)
        if self.type in (0, 1):
            s += " connsig=%08X" % self.connsig
        if self.type == 2:
            s += " frag=%d" % self.frag
        return s + " payload=%d" % len(self.payload)


def parse(dgram: bytes, cklen: int):
    """One PRUDP v0 (Quazal) packet at the start of dgram -> (Packet, bytes used). cklen = checksum size."""
    p = Packet()
    p.src, p.dst, tf, p.session, p.sig, p.seq = struct.unpack_from("<BBBBIH", dgram)
    p.type, p.flags = tf & 7, tf >> 3
    o = 10
    if p.type in (0, 1):
        p.connsig = struct.unpack_from("<I", dgram, o)[0]
        o += 4
    elif p.type == 2:
        p.frag = dgram[o]
        o += 1
    if p.flags & F_HAS_SIZE:
        n = struct.unpack_from("<H", dgram, o)[0]
        o += 2
    else:
        n = len(dgram) - o - cklen
    p.payload = dgram[o:o + n]
    return p, o + n + cklen


def build(p: Packet, cklen: int, base: int) -> bytes:
    b = struct.pack("<BBBBIH", p.src, p.dst, p.type | (p.flags << 3), p.session, p.sig, p.seq)
    if p.type in (0, 1):
        b += struct.pack("<I", p.connsig)
    elif p.type == 2:
        b += bytes([p.frag])
    if p.flags & F_HAS_SIZE:
        b += struct.pack("<H", len(p.payload))
    b += p.payload
    return b + (bytes([checksum1(b, base)]) if cklen == 1 else struct.pack("<I", checksum4(b, base)))


def strings_in(b: bytes):
    """Quazal strings (u16 length including the NUL, then text) found anywhere in a buffer: a reading aid."""
    out, i = [], 0
    while i + 3 <= len(b):
        n = struct.unpack_from("<H", b, i)[0]
        s = b[i + 2:i + 2 + n]
        if 2 <= n <= 256 and len(s) == n and s[-1] == 0 and all(32 <= c < 127 for c in s[:-1]):
            out.append((i, s[:-1].decode()))
            i += 2 + n
        else:
            i += 1
    return out


def decode_payload(raw: bytes, key: bytes):
    """Wire payload -> plain bytes: RC4, then a leading compression byte (0 = stored, else zlib)."""
    d = rc4(key, raw)
    if not d:
        return d, "empty"
    if d[0] == 0:
        return d[1:], "rc4, stored"
    try:
        return zlib.decompress(d[1:]), "rc4, zlib (ratio byte %d)" % d[0]
    except zlib.error:
        return d, "rc4, first byte 0x%02X is neither 0 nor a zlib stream (wrong key or no encryption?)" % d[0]


def describe_rmc(d: bytes) -> str:
    if len(d) < 13:
        return "too short for an RMC request"
    size, proto = struct.unpack_from("<IB", d)
    if not proto & 0x80:
        return "RMC response/other: size=%d protocol byte=0x%02X body=%s" % (size, proto, d[5:].hex())
    pid = proto & 0x7F
    call, method = struct.unpack_from("<II", d, 5)
    name = PROTOCOLS.get(pid, "?")
    mname = AUTH_METHODS.get(method, "") if pid == 10 else ""
    params = d[13:]
    s = "RMC request size=%d protocol=%d (%s) call=%d method=%d %s params[%d]=%s" % (
        size, pid, name, call, method, mname, len(params), params.hex())
    for off, text in strings_in(params):
        s += "\n        string @%d: \"%s\"" % (off, text)
    return s


class Stub:
    def __init__(self, args):
        self.a = args
        self.socks = []
        for port in sorted({args.port, args.secure_port or args.port}):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind((args.bind, port))
            self.socks.append(s)
        self.log = open(args.log, "a", buffering=1) if args.log else None
        self.t0 = time.time()
        self.base = key_sum(args.key)
        self.cklen = 1
        self.conns = {}      # our connection signature -> dict(addr, sock, id, client_sig, seq)
        self.next_id = 0x51A70000
        self.accounts = {"guest": (100, GUEST_PASSWORD)}
        for n, acc in enumerate(args.account or []):
            name, _, pw = acc.partition(":")
            self.accounts[name] = (1000 + n, pw)

    def out(self, s):
        line = "%8.3f %s" % (time.time() - self.t0, s)
        print(line, flush=True)
        if self.log:
            self.log.write(line + "\n")

    def send(self, c, p: Packet, why: str):
        b = build(p, self.cklen, self.base)
        c["sock"].sendto(b, c["addr"])
        self.out("  TX %s:%d %d bytes %s  (%s)\n           %s" % (c["addr"][0], c["addr"][1], len(b), b.hex(), why, p.describe()))

    def ack(self, p: Packet, c, payload=b""):
        r = Packet()
        r.src, r.dst, r.type, r.session, r.seq, r.frag = p.dst, p.src, p.type, p.session, p.seq, p.frag
        r.flags = F_ACK | (p.flags & F_HAS_SIZE)
        r.sig = c.get("client_sig", 0)
        r.payload = payload
        return r

    def run(self):
        self.out("rdv_stub listening on %s, ports %s, access key '%s' (byte sum 0x%02X)%s" % (
            self.a.bind, [s.getsockname()[1] for s in self.socks], self.a.key, self.base,
            ", answering Login for %s" % sorted(self.accounts) if self.a.answer else ", acknowledging only"))
        keys = [self.a.key, "gDg59xT"]
        while True:
            for sock in select.select(self.socks, [], [])[0]:
                dgram, addr = sock.recvfrom(4096)
                self.out("RX %s:%d (to port %d) %d bytes %s" % (addr[0], addr[1], sock.getsockname()[1], len(dgram), dgram.hex()))
                if len(dgram) < 11:
                    self.out("  too short for PRUDP")
                    continue
                self.out("  checksum: " + checksum_report(dgram, keys))
                if (dgram[2] & 7) == 0:  # a SYN has no payload: its length tells the checksum size
                    self.cklen = 4 if len(dgram) >= 18 else 1
                try:
                    p, used = parse(dgram, self.cklen)
                except struct.error as e:
                    self.out("  parse error: %s" % e)
                    continue
                self.out("  %s%s" % (p.describe(), "" if used == len(dgram) else " (+%d more bytes in the datagram)" % (len(dgram) - used)))
                if p.flags & F_ACK:
                    continue
                try:
                    self.handle(sock, addr, p)
                except Exception as e:  # a capture tool: never die on a packet
                    self.out("  handler error: %r" % e)

    def handle(self, sock, addr, p: Packet):
        if p.type == 0:  # SYN -> SYN|ACK carrying our connection signature
            self.next_id += 1
            c = self.conns[self.next_id] = {"addr": addr, "sock": sock, "id": self.next_id, "seq": 0}
            r = self.ack(p, c)
            r.sig = p.sig
            r.connsig = c["id"]
            self.send(c, r, "SYN ack, our connection signature %08X" % r.connsig)
            return
        c = self.conns.get(p.sig)
        if c is None or c["addr"] != addr:
            self.out("  no connection with signature %08X from this address: ignored" % p.sig)
            return
        if p.type == 1:  # CONNECT -> CONNECT|ACK signed with the client's connection signature
            c["client_sig"] = p.connsig
            reply = b""
            if p.payload:
                d, how = decode_payload(p.payload, b"CD&ML")
                self.out("  CONNECT payload (%s): %s" % (how, d.hex()))
                reply = self.secure_connect(d)
            r = self.ack(p, c, reply)
            r.connsig = c["id"]
            self.send(c, r, "CONNECT ack")
        elif p.type == 2:
            d, how = decode_payload(p.payload, b"CD&ML")
            self.out("  DATA payload (%s): %s" % (how, d.hex()))
            self.out("    " + describe_rmc(d))
            self.send(c, self.ack(p, c), "DATA ack")
            if self.a.answer and p.frag == 0 and len(d) >= 13 and d[4] & 0x80:
                call, method = struct.unpack_from("<II", d, 5)
                self.rmc(c, p, d[4] & 0x7F, call, method, d[13:])
        elif p.type in (3, 4):
            self.send(c, self.ack(p, c), "%s ack" % TYPES[p.type])
            if p.type == 3:
                self.conns.pop(p.sig, None)

    @staticmethod
    def internal_ticket() -> bytes:
        return b"stub-ticket-not-a-real-one-0123456789"[:36]

    def secure_connect(self, d: bytes) -> bytes:
        """CONNECT payload of the secure connection: Buffer ticket, Buffer RC4(session key, pid cid check) + HMAC."""
        try:
            n = struct.unpack_from("<I", d)[0]
            ticket = d[4:4 + n]
            m = struct.unpack_from("<I", d, 4 + n)[0]
            check = d[8 + n:8 + n + m]
            plain = rc4(SESSION_KEY, check[:-16])
            mac_ok = hmac.new(SESSION_KEY, check[:-16], hashlib.md5).digest() == check[-16:]
            pid, cid, value = struct.unpack_from("<III", plain)
            self.out("  secure CONNECT: ticket[%d] %s; check data decrypts (session key) to pid=%d cid=%d value=%08X, HMAC %s" % (
                n, "is the one issued" if ticket == self.internal_ticket() else ticket.hex(), pid, cid, value,
                "ok" if mac_ok else "MISMATCH"))
            if not self.a.answer:
                return b""
            return rc4(b"CD&ML", b"\0" + qbuf(struct.pack("<I", (value + 1) & 0xFFFFFFFF)))
        except struct.error as e:
            self.out("  secure CONNECT payload not understood: %s" % e)
            return b""

    def ticket(self, pid: int, password: str) -> bytes:
        key = derive_key(pid, password)
        enc = rc4(key, SESSION_KEY + struct.pack("<I", SERVER_PID) + qbuf(self.internal_ticket()))
        return enc + hmac.new(key, enc, hashlib.md5).digest()

    def rmc(self, c, p: Packet, protocol, call, method, params):
        if protocol == 10 and method in (1, 2):
            n = struct.unpack_from("<H", params)[0]
            name = params[2:2 + n - 1].decode("latin1")
            acc = self.accounts.get(name)
            if acc is None and self.a.unknown == "retval":  # a normal answer whose %retval% is the error
                body = struct.pack("<II", 0x80030064, 0) + qbuf(b"") + qstr("prudp:/") + struct.pack("<I", 0) + qstr("") + qstr("")
                return self.rmc_ok(c, p, protocol, call, method, body, "Login answer with retval InvalidUsername for '%s'" % name)
            if acc is None:
                return self.rmc_error(c, p, protocol, call, 0x80030064, "RendezVous::InvalidUsername for '%s'" % name)
            pid, password = acc
            ticket = self.ticket(pid, password)
            url = "prudps:/address=%s;port=%d;CID=1;PID=%d;sid=1;stream=3;type=2" % (
                self.a.public_ip, self.a.secure_port or self.a.port, SERVER_PID)
            body = struct.pack("<II", OK, pid) + qbuf(ticket) + qstr(url) + struct.pack("<I", 0) + qstr("") + qstr("")
            return self.rmc_ok(c, p, protocol, call, method, body, "Login ok: '%s' pid %d, secure server %s" % (name, pid, url))
        if protocol == 10 and method == 3:  # RequestTicket(source pid, target pid): the ticket for the secure server
            src, dst = struct.unpack_from("<II", params)
            acc = next((v for v in self.accounts.values() if v[0] == src), None)
            if acc is None:
                return self.rmc_error(c, p, protocol, call, 0x80030064, "no principal %d" % src)
            body = struct.pack("<I", OK) + qbuf(self.ticket(*acc))
            return self.rmc_ok(c, p, protocol, call, method, body, "RequestTicket ok: source %d target %d" % (src, dst))
        if protocol == 25 and method in (1, 21, 26):  # a create-account call: remember name and password for this run
            found = [t for _, t in strings_in(params)]
            if len(found) >= 2 and len(found[1]) == 32:
                self.accounts[found[0]] = (1000 + len(self.accounts), "md5x65000:" + found[1])
                why = "account '%s' created for this run (key = the 32 hex digits in the request)" % found[0]
            else:
                why = "create-account call not understood, answered ok anyway"
            return self.rmc_ok(c, p, protocol, call, method, struct.pack("<I", OK), why)
        if protocol == 11 and method in (1, 4):
            url = "prudp:/address=%s;port=%d;sid=15;type=3" % c["addr"]
            body = struct.pack("<II", OK, c["id"] & 0xFFFF) + qstr(url)
            return self.rmc_ok(c, p, protocol, call, method, body,
                               "Register ok: connection id %d, public URL %s" % (c["id"] & 0xFFFF, url))
        return self.rmc_error(c, p, protocol, call, 0x80010002, "Core::NotImplemented")

    def data(self, c, p: Packet, plain: bytes, why: str):
        r = Packet()
        r.src, r.dst, r.type, r.session = p.dst, p.src, 2, p.session
        r.flags = F_RELIABLE | F_NEED_ACK | (p.flags & F_HAS_SIZE)
        r.sig = c.get("client_sig", 0)
        c["seq"] += 1
        r.seq = c["seq"]
        r.payload = rc4(b"CD&ML", b"\0" + plain)
        self.send(c, r, why)

    def rmc_ok(self, c, p, protocol, call, method, body, why):
        msg = struct.pack("<BBII", protocol, 1, call, method | 0x8000) + body
        self.data(c, p, struct.pack("<I", len(msg)) + msg, why)

    def rmc_error(self, c, p, protocol, call, code, why):
        msg = struct.pack("<BBII", protocol, 0, code, call)
        self.data(c, p, struct.pack("<I", len(msg)) + msg, "RMC error 0x%08X (%s) for call %d" % (code, why, call))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="", help="local address to listen on (default: all interfaces)")
    ap.add_argument("--port", type=int, default=31120)
    ap.add_argument("--key", default="3wyEgW4x", help="Rendez-Vous access key of the title")
    ap.add_argument("--log", default="")
    ap.add_argument("--answer", action="store_true", help="answer Login, the secure CONNECT and Register (see above)")
    ap.add_argument("--account", action="append", metavar="NAME:PASSWORD", help="an account --answer knows (repeatable)")
    ap.add_argument("--unknown", choices=("error", "retval"), default="error",
                    help="unknown name at Login: an RMC error, or a normal answer whose retval is the error")
    ap.add_argument("--secure-port", type=int, default=0, help="port named in the ticket's secure server URL (default: --port)")
    ap.add_argument("--public-ip", default="localhost",
                    help="host named in the secure server URL (resolved to IPv4 at start)")
    ap.add_argument("--verify", metavar="HEX", help="check the checksum of one captured datagram and exit")
    a = ap.parse_args()
    try:
        a.public_ip = socket.gethostbyname(a.public_ip)  # the ticket's URL carries a numeric address
    except OSError as e:
        ap.error("--public-ip %s does not resolve (%s)" % (a.public_ip, e))
    if a.verify:
        d = bytes.fromhex(a.verify)
        print(checksum_report(d, [a.key, "gDg59xT"]))
        for cl in (1, 4):
            try:
                p, used = parse(d, cl)
                print("as %d-byte checksum: %s (used %d of %d)" % (cl, p.describe(), used, len(d)))
            except struct.error:
                pass
        return 0
    try:
        Stub(a).run()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
