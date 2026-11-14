"""PRUDP / RMC / ticket encode and decode against datagrams of the game (docs/plan-online.md, section 2).

SYN, CONNECT, LOGIN_GUEST, REQUEST_TICKET, DISCONNECT and FRIENDS_POLL_PLAIN are captured from the game as sent.
LOGIN, REGISTER and CREATE_ACCOUNT are made by this codec with the header fields and call ids of the captured ones and
made-up contents (a test name, a placeholder host in the station URL), so no real account or address is kept here.
"""
import struct
import unittest

import support  # noqa: F401  (puts the package on the path)
from rdvd import codec, kerberos
from rdvd.codec import Packet, Reader, Writer

BASE = codec.key_sum("3wyEgW4x")

# Sent by the game (Steam build 385319) to the capture stub.
SYN = bytes.fromhex("3f3120000000000000000000000062")
CONNECT = bytes.fromhex("3f3131010100a75101002879b014d4")
LOGIN = bytes.fromhex("3f3132010100a7510200000f9244db1375295005a260fd2a16f8b1ad378590e509ed723fef9b")  # made here
LOGIN_GUEST = bytes.fromhex("3f3132950200a7510200000f9644db13752a5005a260fd2a16f4b198238386e356b9")
REQUEST_TICKET = bytes.fromhex("3f3132950200a7510300000f9644db1375265005a262fd2a1696b1ff56e4f5975678")
REGISTER = bytes.fromhex("3f3132dc0300a7510200000fb944db1374215005a260fd2a16f3b1ff56c9f5e724d7782ad59a66eebe326b050ff874bffa62"
                         "c0e6c1ce9d5d38ebcb678fe17bc9bda1f82387d1a0ed1088007a8ddf94")  # made here
CREATE_ACCOUNT = bytes.fromhex("3f3132dc0300a7510300000fd744db13663d5005a260fd2a16f8b1ad378590e509ed723fef9407b3e8223c434bf165"
                               "b5fa2d91a38aced05d67fa9467d0f32189acf9a0768fd7ad9e59ec3d4baadf29cf3ec1b82592854827742eaccdd1a1"
                               "cbe60d")  # made here
FRIENDS_POLL_PLAIN = bytes.fromhex("0b000000940e0000000a0000000201")
DISCONNECT = bytes.fromhex("3f3133660200a7510400d9")
# A password typed in a capture and the key string the game sent for it at account creation (CREATE_ACCOUNT carries it).
NAME, STATION_URL = "Racer_One", "prudp:/address=player.example;port=9103;sid=15"
PASSWORD, KEY_HEX = "wsqeqe", "92b2574afa647ee5a2a3edb52eead856"


class Checksum(unittest.TestCase):
    def test_access_key_sum(self):
        self.assertEqual(BASE, 0xD2)

    def test_captured_packets_verify(self):
        for d in (SYN, CONNECT, LOGIN, LOGIN_GUEST, REQUEST_TICKET, REGISTER, CREATE_ACCOUNT, DISCONNECT):
            self.assertEqual(codec.checksum(d[:-1], BASE), d[-1])

    def test_other_key_fails(self):
        with self.assertRaises(codec.WireError):
            codec.decode_packets(LOGIN, codec.key_sum("gDg59xT"))

    def test_corruption_fails(self):
        bad = bytearray(LOGIN)
        bad[20] ^= 1
        with self.assertRaises(codec.WireError):
            codec.decode_packets(bytes(bad), BASE)


class Packets(unittest.TestCase):
    def test_syn(self):
        (p,) = codec.decode_packets(SYN, BASE)
        self.assertEqual((p.type, p.flags, p.src, p.dst, p.seq, p.connsig), (codec.SYN, codec.F_NEED_ACK, 0x3F, 0x31, 0, 0))

    def test_connect(self):
        (p,) = codec.decode_packets(CONNECT, BASE)
        self.assertEqual((p.type, p.flags, p.session, p.sig, p.seq, p.connsig, p.payload),
                         (codec.CONNECT, codec.F_RELIABLE | codec.F_NEED_ACK, 1, 0x51A70001, 1, 0x14B07928, b""))

    def test_roundtrip_is_byte_exact(self):
        for d in (SYN, CONNECT, LOGIN_GUEST, REQUEST_TICKET, LOGIN, REGISTER, CREATE_ACCOUNT, DISCONNECT):
            (p,) = codec.decode_packets(d, BASE)
            self.assertEqual(p.encode(BASE), d)

    def test_two_packets_in_one_datagram(self):
        a = Packet(codec.DATA, codec.F_HAS_SIZE | codec.F_RELIABLE, seq=2, payload=b"abc").encode(BASE)
        b = Packet(codec.PING, codec.F_HAS_SIZE | codec.F_NEED_ACK, seq=9).encode(BASE)
        ps = codec.decode_packets(a + b, BASE)
        self.assertEqual([(p.type, p.seq, p.payload) for p in ps], [(codec.DATA, 2, b"abc"), (codec.PING, 9, b"")])

    def test_garbage_never_escapes_as_another_exception(self):
        import random
        rnd = random.Random(1)
        for _ in range(3000):
            d = bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 60)))
            try:
                codec.decode_packets(d, BASE)
            except codec.WireError:
                pass
        for src in (LOGIN, REGISTER, CREATE_ACCOUNT):
            for _ in range(500):
                d = bytearray(src)
                d[rnd.randrange(len(d))] = rnd.randrange(256)
                try:
                    for p in codec.decode_packets(bytes(d[:rnd.randrange(1, len(d) + 1)]), BASE):
                        m = codec.unwrap_payload(p.payload)
                        if m:
                            codec.decode_message(m)
                except codec.WireError:
                    pass


class Messages(unittest.TestCase):
    def request(self, dgram):
        (p,) = codec.decode_packets(dgram, BASE)
        return codec.decode_message(codec.unwrap_payload(p.payload))

    def test_login(self):
        q = self.request(LOGIN)
        self.assertEqual((q.protocol, q.call, q.method), (10, 5, 1))
        self.assertEqual(Reader(q.params).string(), NAME)

    def test_request_ticket(self):
        q = self.request(REQUEST_TICKET)
        self.assertEqual((q.protocol, q.method, struct.unpack("<II", q.params)), (10, 3, (100, 2)))

    def test_register(self):
        q = self.request(REGISTER)
        self.assertEqual((q.protocol, q.method), (11, 1))
        self.assertEqual(Reader(q.params).list(lambda r: r.string()), [STATION_URL])

    def test_create_account(self):
        q = self.request(CREATE_ACCOUNT)
        r = Reader(q.params)
        self.assertEqual((q.protocol, q.method), (25, 1))
        self.assertEqual((r.string(), r.string(), r.u32(), r.string()), (NAME, KEY_HEX, 0x20, "racer@example.com"))

    def test_friends_poll(self):
        q = codec.decode_message(FRIENDS_POLL_PLAIN)
        self.assertEqual((q.protocol, q.method, q.params), (20, 10, b"\x02\x01"))

    def test_wrap_matches_the_game(self):
        (p,) = codec.decode_packets(LOGIN_GUEST, BASE)                     # captured: the game's own bytes
        plain = codec.unwrap_payload(p.payload)
        self.assertEqual(codec.wrap_payload(plain), p.payload)
        self.assertEqual(codec.encode_request(10, 6, 1, Writer().string("guest").bytes()), plain)

    def test_answers(self):
        ok = codec.encode_success(10, 5, 1, b"\x01\x02")
        self.assertEqual(codec.decode_response(ok), (10, True, 5, 1, b"\x01\x02"))
        err = codec.encode_error(10, 5, codec.ERR_NOT_IMPLEMENTED)
        self.assertEqual(codec.decode_response(err), (10, False, 5, 0x80010002, b""))
        # byte for byte what the stub sent and the game accepted as an error answer
        self.assertEqual(err.hex(), "0a0000000a000200018005000000")

    def test_compressed_payload(self):
        import zlib
        plain = codec.encode_request(21, 7, 17, b"x" * 400)
        wire = codec.rc4(codec.DEFAULT_KEY, bytes([5]) + zlib.compress(plain))
        self.assertEqual(codec.unwrap_payload(wire), plain)

    def test_inflate_bomb_refused(self):
        import zlib
        wire = codec.rc4(codec.DEFAULT_KEY, bytes([200]) + zlib.compress(b"\0" * (codec.MAX_INFLATE * 4)))
        with self.assertRaises(codec.WireError):
            codec.unwrap_payload(wire)

    def test_anydata(self):
        w = Writer().anydata("ATVGameSession", b"12345")
        self.assertEqual(Reader(w.bytes()).anydata(), ("ATVGameSession", b"12345"))

    def test_reader_limits(self):
        with self.assertRaises(codec.WireError):
            Reader(struct.pack("<H", 5) + b"ab").string()
        with self.assertRaises(codec.WireError):
            Reader(struct.pack("<I", 1 << 30)).list(lambda r: r.u32())

    def test_urls(self):
        scheme, f = codec.parse_url(STATION_URL)
        self.assertEqual((scheme, f), ("prudp", {"address": "player.example", "port": "9103", "sid": "15"}))
        self.assertEqual(codec.make_url(scheme, f), STATION_URL)


class Tickets(unittest.TestCase):
    def test_the_game_sends_md5_x_65000(self):
        self.assertEqual(kerberos.password_hash(PASSWORD).hex(), KEY_HEX)

    def test_ticket_roundtrip(self):
        key = kerberos.principal_key(1001, bytes.fromhex(KEY_HEX))
        t = kerberos.make_ticket(b"s" * 32, 1001, key, now=50.0)
        session_key, server_pid, internal = kerberos.open_ticket(key, t)
        self.assertEqual((len(session_key), server_pid), (16, kerberos.SERVER_PID))
        self.assertEqual(kerberos.open_internal_ticket(b"s" * 32, internal, now=60.0), (1001, session_key))

    def test_wrong_key_does_not_open(self):
        t = kerberos.make_ticket(b"s" * 32, 1001, kerberos.principal_key(1001, bytes.fromhex(KEY_HEX)))
        with self.assertRaises(codec.WireError):
            kerberos.open_ticket(kerberos.principal_key(1001, kerberos.password_hash("other")), t)

    def test_internal_ticket_is_ours_only(self):
        t = kerberos.make_internal_ticket(b"s" * 32, 1001, b"k" * 16, now=50.0)
        with self.assertRaises(codec.WireError):
            kerberos.open_internal_ticket(b"t" * 32, t, now=60.0)                       # another secret
        with self.assertRaises(codec.WireError):
            kerberos.open_internal_ticket(b"s" * 32, t, now=50.0 + kerberos.TICKET_LIFETIME + 1)   # expired
        forged = bytearray(t)
        forged[12] ^= 1                                                                # the pid field
        with self.assertRaises(codec.WireError):
            kerberos.open_internal_ticket(b"s" * 32, bytes(forged), now=60.0)

    def test_connect_check(self):
        c = kerberos.make_connect_check(b"k" * 16, 1000, 0, 0x81F0EEAF)
        self.assertEqual(len(c), 28)       # the length seen in the capture
        self.assertEqual(kerberos.open_connect_check(b"k" * 16, c), (1000, 0, 0x81F0EEAF))
        with self.assertRaises(codec.WireError):
            kerberos.open_connect_check(b"j" * 16, c)


if __name__ == "__main__":
    unittest.main()
