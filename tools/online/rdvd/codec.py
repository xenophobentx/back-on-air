"""Wire formats: PRUDP v0 (Quazal variant) packets, payload processing, RMC messages and their value types.

Everything here is pure functions and small classes, no sockets and no state, so that it can be tested offline
against captured datagrams. Layouts were confirmed against the game's own traffic (docs/plan-online.md, section 2).
"""
import struct
import zlib

SYN, CONNECT, DATA, DISCONNECT, PING, NATPING = 0, 1, 2, 3, 4, 5
TYPE_NAMES = {SYN: "SYN", CONNECT: "CONNECT", DATA: "DATA", DISCONNECT: "DISCONNECT", PING: "PING", NATPING: "NATPING"}
F_ACK, F_RELIABLE, F_NEED_ACK, F_HAS_SIZE = 1, 2, 4, 8

DEFAULT_KEY = b"CD&ML"          # the library's default RC4 key; this client uses it on both connections
MAX_FRAGMENT = 900              # payload bytes of one DATA packet of ours (the library's limit is about 960)
MAX_INFLATE = 256 * 1024        # an inflated payload larger than this is refused

OK = 0x00010001                 # Core::NoError, as a %retval%
ERR_NOT_IMPLEMENTED = 0x80010002
ERR_ACCESS_DENIED = 0x80010006
ERR_INVALID_ARGUMENT = 0x8001000A
ERR_INVALID_USERNAME = 0x80030064
ERR_INVALID_PASSWORD = 0x80030065
ERR_USERNAME_EXISTS = 0x80030066
ERR_ACCOUNT_DISABLED = 0x80030067
ERR_CONCURRENT_LOGIN = 0x80030069
ERR_INVALID_GID = 0x8003006D
ERR_SESSION_FULL = 0x800300C8
ERR_PERMISSION_DENIED = 0x800300D9


class WireError(Exception):
    """A datagram or message that does not parse. Never fatal: the caller drops the packet."""


def rc4(key: bytes, data: bytes) -> bytes:
    s = list(range(256))
    j = 0
    n = len(key)
    for i in range(256):
        j = (j + s[i] + key[i % n]) & 255
        s[i], s[j] = s[j], s[i]
    out = bytearray(len(data))
    i = j = 0
    for k, b in enumerate(data):
        i = (i + 1) & 255
        j = (j + s[i]) & 255
        s[i], s[j] = s[j], s[i]
        out[k] = b ^ s[(s[i] + s[j]) & 255]
    return bytes(out)


def key_sum(access_key: str) -> int:
    return sum(access_key.encode("ascii")) & 0xFF


def checksum(data: bytes, base: int) -> int:
    """One byte: the bytes of the little-endian u32 word sum, plus the trailing bytes, plus the access key's sum."""
    n = len(data) & ~3
    total = sum(struct.unpack_from("<%dI" % (n // 4), data)) & 0xFFFFFFFF
    return (base + sum(data[n:]) + sum(struct.pack("<I", total))) & 0xFF


class Packet:
    __slots__ = ("src", "dst", "type", "flags", "session", "sig", "seq", "connsig", "frag", "payload")

    def __init__(self, type=DATA, flags=0, src=0x31, dst=0x3F, session=0, sig=0, seq=0, connsig=0, frag=0, payload=b""):
        self.src, self.dst, self.type, self.flags, self.session = src, dst, type, flags, session
        self.sig, self.seq, self.connsig, self.frag, self.payload = sig, seq, connsig, frag, payload

    def __repr__(self):
        fl = "|".join(n for b, n in ((1, "ACK"), (2, "REL"), (4, "NEED_ACK"), (8, "SIZE")) if self.flags & b)
        return "%s[%s] %02X>%02X ses=%d sig=%08X seq=%d%s len=%d" % (
            TYPE_NAMES.get(self.type, self.type), fl, self.src, self.dst, self.session, self.sig, self.seq,
            " csig=%08X" % self.connsig if self.type in (SYN, CONNECT) else (" frag=%d" % self.frag if self.type == DATA else ""),
            len(self.payload))

    def encode(self, base: int) -> bytes:
        b = struct.pack("<BBBBIH", self.src, self.dst, self.type | (self.flags << 3), self.session, self.sig, self.seq)
        if self.type in (SYN, CONNECT):
            b += struct.pack("<I", self.connsig)
        elif self.type == DATA:
            b += bytes([self.frag])
        if self.flags & F_HAS_SIZE:
            b += struct.pack("<H", len(self.payload))
        b += self.payload
        return b + bytes([checksum(b, base)])


def decode_packets(dgram: bytes, base: int):
    """All packets of one datagram. Raises WireError on a short packet or a wrong checksum."""
    out = []
    o = 0
    while o < len(dgram):
        if len(dgram) - o < 11:
            raise WireError("short packet")
        p = Packet()
        p.src, p.dst, tf, p.session, p.sig, p.seq = struct.unpack_from("<BBBBIH", dgram, o)
        p.type, p.flags = tf & 7, tf >> 3
        h = o + 10
        if p.type in (SYN, CONNECT):
            if len(dgram) - h < 5:
                raise WireError("short packet")
            p.connsig = struct.unpack_from("<I", dgram, h)[0]
            h += 4
        elif p.type == DATA:
            p.frag = dgram[h]
            h += 1
        if p.flags & F_HAS_SIZE:
            if len(dgram) - h < 3:
                raise WireError("short packet")
            n = struct.unpack_from("<H", dgram, h)[0]
            h += 2
            if h + n + 1 > len(dgram):
                raise WireError("size field past the datagram")
        else:
            n = len(dgram) - h - 1
            if n < 0:
                raise WireError("short packet")
        end = h + n
        if checksum(dgram[o:end], base) != dgram[end]:
            raise WireError("checksum")
        p.payload = dgram[h:end]
        out.append(p)
        o = end + 1
    return out


def unwrap_payload(raw: bytes, key: bytes = DEFAULT_KEY) -> bytes:
    """Wire payload -> message bytes: RC4, then a compression byte (0 = stored, else zlib)."""
    if not raw:
        return b""
    d = rc4(key, raw)
    if d[0] == 0:
        return d[1:]
    z = zlib.decompressobj()
    try:
        out = z.decompress(d[1:], MAX_INFLATE)
    except zlib.error as e:
        raise WireError("inflate: %s" % e)
    if z.unconsumed_tail:
        raise WireError("inflated payload too large")
    return out


def wrap_payload(plain: bytes, key: bytes = DEFAULT_KEY) -> bytes:
    """Message bytes -> wire payload, always stored (the client accepts that)."""
    return rc4(key, b"\0" + plain)


# ---------------------------------------------------------------------------------------------- RMC value streams

class Reader:
    def __init__(self, data: bytes, pos: int = 0):
        self.d, self.o = data, pos

    def _take(self, n: int) -> bytes:
        if n < 0 or self.o + n > len(self.d):
            raise WireError("message ends early")
        b = self.d[self.o:self.o + n]
        self.o += n
        return b

    def u8(self): return self._take(1)[0]
    def u16(self): return struct.unpack("<H", self._take(2))[0]
    def u32(self): return struct.unpack("<I", self._take(4))[0]
    def u64(self): return struct.unpack("<Q", self._take(8))[0]
    def bool(self): return self.u8() != 0

    def string(self, limit: int = 1024) -> str:
        n = self.u16()
        if n > limit:
            raise WireError("string too long")
        b = self._take(n)
        return b.split(b"\0", 1)[0].decode("utf-8", "replace")

    def buffer(self, limit: int = 4096) -> bytes:
        n = self.u32()
        if n > limit:
            raise WireError("buffer too long")
        return self._take(n)

    def list(self, item, limit: int = 64):
        n = self.u32()
        if n > limit:
            raise WireError("list too long")
        return [item(self) for _ in range(n)]

    def anydata(self, limit: int = 4096):
        """AnyDataHolder: class name, u32 length + 4, Buffer -> (class name, content bytes)."""
        name = self.string(64)
        outer = self.u32()
        body = self.buffer(limit)
        if outer != len(body) + 4:
            raise WireError("AnyDataHolder lengths disagree")
        return name, body

    def rest(self) -> bytes:
        b = self.d[self.o:]
        self.o = len(self.d)
        return b


class Writer:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v): self.b.append(v & 0xFF); return self
    def u16(self, v): self.b += struct.pack("<H", v & 0xFFFF); return self
    def u32(self, v): self.b += struct.pack("<I", v & 0xFFFFFFFF); return self
    def u64(self, v): self.b += struct.pack("<Q", v); return self
    def bool(self, v): return self.u8(1 if v else 0)
    def raw(self, v: bytes): self.b += v; return self

    def string(self, s: str):
        e = s.encode("utf-8") + b"\0"
        self.b += struct.pack("<H", len(e)) + e
        return self

    def buffer(self, v: bytes):
        self.b += struct.pack("<I", len(v)) + v
        return self

    def list(self, items, put):
        self.u32(len(items))
        for i in items:
            put(self, i)
        return self

    def anydata(self, name: str, body: bytes):
        return self.string(name).u32(len(body) + 4).buffer(body)

    def bytes(self) -> bytes:
        return bytes(self.b)


class Request:
    __slots__ = ("protocol", "call", "method", "params")

    def __init__(self, protocol, call, method, params):
        self.protocol, self.call, self.method, self.params = protocol, call, method, params


def decode_message(d: bytes):
    """An RMC message -> Request, or None for anything that is not a request (answers to our own calls)."""
    if len(d) < 5:
        raise WireError("short RMC message")
    size, proto = struct.unpack_from("<IB", d)
    if size != len(d) - 4:
        raise WireError("RMC size field")
    if not proto & 0x80:
        return None
    if len(d) < 13:
        raise WireError("short RMC request")
    call, method = struct.unpack_from("<II", d, 5)
    return Request(proto & 0x7F, call, method, d[13:])


def encode_request(protocol: int, call: int, method: int, params: bytes) -> bytes:
    body = struct.pack("<BII", protocol | 0x80, call, method) + params
    return struct.pack("<I", len(body)) + body


def encode_success(protocol: int, call: int, method: int, body: bytes) -> bytes:
    m = struct.pack("<BBII", protocol, 1, call, method | 0x8000) + body
    return struct.pack("<I", len(m)) + m


def encode_error(protocol: int, call: int, code: int) -> bytes:
    m = struct.pack("<BBII", protocol, 0, code | 0x80000000, call)
    return struct.pack("<I", len(m)) + m


def decode_response(d: bytes):
    """For the test client: -> (protocol, ok, call, method or error code, body)."""
    size, proto, ok = struct.unpack_from("<IBB", d)
    if ok:
        call, method = struct.unpack_from("<II", d, 6)
        return proto, True, call, method & 0x7FFF, d[14:]
    code, call = struct.unpack_from("<II", d, 6)
    return proto, False, call, code, b""


# ---------------------------------------------------------------------------------------------------- station URLs

def parse_url(url: str):
    """"prudp:/address=HOST;port=9103;sid=15" -> ("prudp", {"address": "HOST", "port": "9103", "sid": "15"})."""
    scheme, _, rest = url.partition(":/")
    fields = {}
    for part in rest.split(";"):
        k, eq, v = part.partition("=")
        if eq:
            fields[k] = v
    return scheme, fields


def make_url(scheme: str, fields: dict) -> str:
    return scheme + ":/" + ";".join("%s=%s" % kv for kv in fields.items())
