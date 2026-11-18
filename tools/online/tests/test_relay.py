"""The relay port (wire format "SR" v2, Takedawn's relay model): registration, binding to a session, forwarding only
inside a gathering, peer endpoints for the direct try, no reflection, no amplification, limits."""
import hashlib
import hmac
import struct
import unittest
from unittest import mock

from support import Net
from rdvd import relay
from rdvd.codec import Writer
from rdvd.testclient import atv_session

PUB = {"player1.test": bytes([203, 0, 113, 1]), "player2.test": bytes([203, 0, 113, 2])}


def hello_bytes(nonce, vip, lan=bytes(6), old_key=None):
    body = b"SR\x02\x01" + nonce + vip + lan
    proof = relay.mac(old_key, body) if old_key else bytes(8)
    return (body + proof).ljust(relay.HELLO_LEN, b"\0")


class RelayTest(unittest.TestCase):
    def setUp(self):
        self.net = Net()
        self.addCleanup(self.net.close)
        self.r = self.net.server.relay
        p = mock.patch.object(relay, "ip4", lambda host: PUB.get(host, bytes(4)))
        p.start()
        self.addCleanup(p.stop)
        self.next_vip = 10

    def out(self, n):
        return self.net.relayed[n:]

    def hello(self, addr, vip, nonce=b"12345678", lan=bytes(6), old_key=None):
        n = len(self.net.relayed)
        self.net.server.datagram("relay", hello_bytes(nonce, vip, lan, old_key), addr, self.net.now)
        out = self.out(n)
        if not out:
            return None
        (data, to), = out
        self.assertEqual((to, data[:4], data[4:12], data[12:16], len(data)), (addr, b"SR\x02\x02", nonce, vip, 39))
        return data[16], data[17:33], data[33:37], struct.unpack(">H", data[37:39])[0]   # status, key, pub ip, port

    def player(self, name, ip, tunnel_port=40000, lan=bytes([192, 168, 0, 5, 0x23, 0x8F])):
        """A signed-in client whose game registered its virtual address, as the DLL makes it do."""
        self.next_vip += 1
        vip = bytes([10, 254, 1, self.next_vip])
        status, key, _pub, _port = self.hello((ip, tunnel_port), vip, lan=lan)
        self.assertEqual(status, 0)
        c = self.net.client(ip)
        c.local_url = "prudp:/address=%s;port=9103;sid=15" % relay.vip_text(vip)
        self.assertEqual(c.create_account(name, "pw"), 0x00010001)
        self.assertEqual(c.login(name, "pw"), "ok")
        return c, vip, key, (ip, tunnel_port)

    def send(self, addr, key, dst_vip, payload, ports=(9103, 9103)):
        n = len(self.net.relayed)
        head = b"SR\x02\x03" + dst_vip + struct.pack(">HH", *ports)
        self.net.server.datagram("relay", head + relay.mac(key, head, payload) + payload, addr, self.net.now)
        return self.out(n)

    def peer(self, addr, key, vip, nonce=b"\0\0\0\7"):
        n = len(self.net.relayed)
        body = b"SR\x02\x05" + vip + nonce
        self.net.server.datagram("relay", (body + relay.mac(key, body)).ljust(52, b"\0"), addr, self.net.now)
        return self.out(n)

    def lobby(self, join=True):
        a = self.player("Host_Player", "player1.test")
        b = self.player("Join_Player", "player2.test", lan=bytes([192, 168, 0, 6, 0x23, 0x8F]))
        gid = struct.unpack("<I", a[0].call(21, 1, atv_session())[1])[0]
        a[0].call(21, 11, Writer().u32(gid).string("").bytes())
        if join:
            b[0].call(21, 11, Writer().u32(gid).string("").bytes())
        return a, b, gid

    def drops(self, why):
        return self.net.server.counters.get("relay_drop_" + why, 0)

    # --------------------------------------------------------------------------------------------- registration
    def test_hello_grants_the_claimed_address_with_a_key_and_the_public_endpoint(self):
        vip = bytes([10, 254, 3, 4])
        status, key, pub, port = self.hello(("player1.test", 40000), vip)
        self.assertEqual((status, len(key), pub, port), (0, 16, PUB["player1.test"], 40000))
        self.assertEqual(self.hello(("player1.test", 40000), vip)[1], key)       # the keep-alive: same registration

    def test_an_address_taken_by_another_tunnel_is_refused(self):
        vip = bytes([10, 254, 3, 4])
        _s, key, _p, _q = self.hello(("player1.test", 40000), vip)
        status, key2, _p, _q = self.hello(("player2.test", 40000), vip)
        self.assertEqual((status, key2), (1, bytes(16)))                          # no key for the second claimer
        self.assertEqual(self.r.by_vip[vip].addr, ("player1.test", 40000))
        status, key3, _p, port = self.hello(("player1.test", 41234), vip, old_key=key)   # NAT changed the port: proof
        self.assertEqual((status, key3, port), (0, key, 41234))
        self.assertEqual(self.r.by_vip[vip].addr, ("player1.test", 41234))

    def test_hello_checks_and_sizes(self):
        for bad in (bytes([10, 254, 3, 0]), bytes([10, 254, 3, 255]), bytes([10, 1, 3, 4])):
            self.assertIsNone(self.hello(("player1.test", 40000), bad))
        n = len(self.net.relayed)
        self.net.server.datagram("relay", hello_bytes(b"12345678", bytes([10, 254, 3, 4]))[:30], ("player1.test", 1), 0)
        self.assertEqual(self.out(n), [])                                         # a short HELLO gets nothing
        self.assertLessEqual(relay.WELCOME_LEN, relay.HELLO_LEN)                  # no amplification
        self.assertLessEqual(relay.PEERINFO_LEN, relay.PEER_LEN)

    def test_hello_rate_per_address(self):
        got = [self.hello(("player1.test", 41000 + i), bytes([10, 254, 5, 1 + i])) for i in range(14)]
        self.assertEqual(sum(1 for g in got if g), min(10, relay.MAX_PER_IP))

    def test_registrations_per_source_ip_are_capped(self):
        for i in range(relay.MAX_PER_IP):
            self.net.now += 3
            self.assertEqual(self.hello(("player1.test", 41000 + i), bytes([10, 254, 5, 1 + i]))[0], 0)
        self.net.now += 3
        self.assertIsNone(self.hello(("player1.test", 42000), bytes([10, 254, 6, 1])))
        self.assertEqual(self.drops("per_ip"), 1)
        self.assertEqual(self.hello(("player2.test", 42000), bytes([10, 254, 6, 1]))[0], 0)   # another address: fine

    def test_a_full_table_frees_the_oldest_registration_without_a_session(self):
        with mock.patch.object(relay, "MAX_REGISTRATIONS", 2):
            self.assertEqual(self.hello(("player1.test", 41000), bytes([10, 254, 5, 1]))[0], 0)
            self.assertEqual(self.hello(("player2.test", 41000), bytes([10, 254, 5, 2]))[0], 0)
            self.assertIsNone(self.hello(("player2.test", 41001), bytes([10, 254, 5, 3])))     # both young: full
            self.net.now += relay.IDLE_EVICT
            self.assertEqual(self.hello(("player2.test", 41001), bytes([10, 254, 5, 3]))[0], 0)
            self.assertNotIn(bytes([10, 254, 5, 1]), self.r.by_vip)

    def test_only_private_lan_addresses_are_passed_on(self):
        self.assertTrue(relay.lan_ok(bytes([192, 168, 1, 2])) and relay.lan_ok(bytes([10, 0, 0, 7])))
        for ip in ([8, 8, 8, 8], [127, 0, 0, 1], [10, 254, 1, 2], [0, 0, 0, 0], [169, 254, 1, 1]):
            self.assertFalse(relay.lan_ok(bytes(ip)), ip)

    # --------------------------------------------------------------------------------------------- the session
    def test_register_announces_only_the_virtual_address(self):
        c, vip, _key, _addr = self.player("Host_Player", "player1.test")
        self.assertEqual(c.public_url, "prudp:/address=%s;port=9103;sid=15;type=3" % relay.vip_text(vip))
        other = self.net.client("player2.test")
        other.create_account("Join_Player", "pw")
        other.login("Join_Player", "pw")
        ok, body = other.call(11, 3, Writer().u32(c.cid).u32(c.pid).bytes())
        self.assertNotIn(b"player1.test", body)
        self.assertIn(relay.vip_text(vip).encode(), body)

    def test_virtual_address_of_another_ip_cannot_be_bound(self):
        vip = bytes([10, 254, 3, 4])
        self.hello(("player1.test", 40000), vip)
        thief = self.net.client("thief.test")
        thief.local_url = "prudp:/address=%s;port=9103;sid=15" % relay.vip_text(vip)
        thief.create_account("Thief_Player", "pw")
        self.assertEqual(thief.login("Thief_Player", "pw"), "ok")
        self.assertEqual(thief.public_url, "prudp:/address=thief.test;port=9103;sid=15;type=3")
        self.assertIsNone(self.r.by_vip[vip].pid)

    # --------------------------------------------------------------------------------------------- forwarding
    def test_forward_inside_a_gathering_both_ways(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby(join=False)
        self.assertEqual(self.send(baddr, bkey, avip, b"early"), [])             # not a participant yet
        self.assertEqual(self.drops("member"), 1)
        b.call(21, 11, Writer().u32(gid).string("").bytes())
        (data, to), = self.send(baddr, bkey, avip, b"hello host", ports=(9104, 9103))
        self.assertEqual((to, data[:4], data[4:8], data[8:12], data[20:]),
                         (aaddr, b"SR\x02\x04", bvip, struct.pack(">HH", 9104, 9103), b"hello host"))
        self.assertEqual(data[12:20], relay.mac(akey, data[:12], b"hello host"))  # re-signed for the receiver
        (data, to), = self.send(aaddr, akey, bvip, b"hello joiner")
        self.assertEqual((to, data[4:8], data[20:]), (baddr, avip, b"hello joiner"))
        c = self.net.server.counters
        self.assertEqual((c["relay_datagrams"], c["relay_bytes"], c["relay_games"]), (2, 22, 1))

    def test_same_size_out_as_in(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby()
        n = len(self.net.relayed)
        head = b"SR\x02\x03" + avip + struct.pack(">HH", 9103, 9103)
        sent = head + relay.mac(bkey, head, b"x" * 1400) + b"x" * 1400
        self.net.server.datagram("relay", sent, baddr, self.net.now)
        (data, _to), = self.out(n)
        self.assertEqual(len(data), len(sent))

    def test_refusals(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby()
        self.assertEqual(self.send(baddr, akey, avip, b"x"), [])                 # another player's key
        self.assertEqual(self.drops("tag"), 1)
        self.assertEqual(self.send(("stranger.test", 1), bkey, avip, b"x"), [])  # unregistered source
        self.assertEqual(self.send(baddr, bkey, bytes([10, 254, 9, 9]), b"x"), [])   # nobody there
        self.assertEqual(self.send(baddr, bkey, bvip, b"x"), [])                 # to itself
        self.assertEqual(self.drops("nodest"), 2)
        n = len(self.net.relayed)
        self.net.server.datagram("relay", b"junk", baddr, self.net.now)
        self.net.server.datagram("relay", b"SR\x01\x03" + avip + bytes(16) + b"x", baddr, self.net.now)   # version 1
        self.net.server.datagram("relay", b"SR\x02\x03" + avip + bytes(16) + b"x" * 1401, baddr, self.net.now)
        self.net.server.datagram("relay", b"SR\x02\x04" + avip + bytes(16) + b"x", baddr, self.net.now)   # DELIVER in
        self.assertEqual(self.out(n), [])
        self.assertEqual(self.drops("bad"), 4)
        self.assertEqual(len(self.send(baddr, bkey, avip, b"ok")), 1)
        b.call(21, 12, Writer().u32(gid).string("").bytes())                    # left the gathering: still allowed
        self.assertEqual(len(self.send(baddr, bkey, avip, b"x")), 1)              # (Takedawn: members are add-only)

    def test_a_pair_outlives_the_lobby_connection_for_30_minutes(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby()
        self.assertEqual(len(self.send(baddr, bkey, avip, b"x")), 1)
        b.links["secure"].disconnect()
        self.assertIsNone(self.r.by_vip[bvip].pid)
        self.assertEqual(len(self.send(baddr, bkey, avip, b"race")), 1)           # the race goes on
        for _ in range(4):                                                        # keep-alives keep it registered
            self.net.advance(20)
            self.hello(aaddr, avip)
            self.hello(baddr, bvip)
        self.r.pairs[frozenset((avip, bvip))] = (self.net.now - relay.PAIR_TTL, gid)
        a.links["secure"].disconnect()
        self.assertEqual(self.send(baddr, bkey, avip, b"late"), [])

    def test_a_reused_address_does_not_inherit_old_pairs(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby()
        self.assertEqual(len(self.send(baddr, bkey, avip, b"x")), 1)
        b.links["secure"].disconnect()
        self.net.advance(relay.REGISTRATION_LIFETIME + 61)
        self.assertNotIn(bvip, self.r.by_vip)
        _s, newkey, _p, _q = self.hello(("player3.test", 40000), bvip)          # somebody else, same address
        self.assertEqual(self.send(("player3.test", 40000), newkey, avip, b"x"), [])

    def test_rate_limit(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby()
        out = sum(len(self.send(baddr, bkey, avip, b"p" * 100)) for _ in range(900))
        self.assertEqual(out, 400)                                               # the burst (one second), then nothing
        self.assertEqual(self.drops("rate"), 500)

    def test_registration_expires(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby()
        self.net.advance(relay.REGISTRATION_LIFETIME + 61)
        self.assertNotIn(bvip, self.r.by_vip)
        self.assertEqual(self.send(baddr, bkey, avip, b"x"), [])

    # --------------------------------------------------------------------------------------------- peer endpoints
    def test_peer_answer_gives_the_endpoints_and_one_pair_key_to_both(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby()
        (data, to), = self.peer(aaddr, akey, bvip)
        self.assertEqual((to, len(data), data[:4], data[4:8], data[8:12]), (aaddr, 48, b"SR\x02\x06", bvip, b"\0\0\0\7"))
        self.assertEqual(data[40:48], relay.mac(akey, data[:40]))
        self.assertEqual((data[12:16], data[16:18]), (PUB["player2.test"], struct.pack(">H", 40000)))
        self.assertEqual(data[18:24], bytes([192, 168, 0, 6, 0x23, 0x8F]))
        (back, _to), = self.peer(baddr, bkey, avip)
        self.assertEqual(back[24:40], data[24:40])                                # the same pair key on both sides
        self.assertEqual(self.net.server.counters["relay_peer_answers"], 2)

    def test_relay_only_player_endpoints_are_never_handed_out(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby()
        n = len(self.net.relayed)
        data = bytearray(hello_bytes(b"12345678", bvip, bytes([192, 168, 0, 6, 0x23, 0x8F])))
        data[30] = relay.HELLO_RELAY_ONLY
        self.net.server.datagram("relay", bytes(data), baddr, self.net.now)
        self.assertEqual(len(self.out(n)), 1)                                     # WELCOME as before
        self.assertEqual(self.peer(aaddr, akey, bvip), [])
        self.assertEqual(self.drops("private"), 1)
        self.assertEqual(self.r.by_vip[bvip].lan, bytes(6))
        self.assertEqual(len(self.peer(baddr, bkey, avip)), 1)                    # it may still ask about others
        self.assertEqual(len(self.send(aaddr, akey, bvip, b"race")), 1)           # and the relay carries it

    def test_tunnelled_and_plain_players_do_not_meet(self):
        (a, avip, akey, aaddr), _b, gid = self.lobby(join=False)
        plain = self.net.client("player3.test")
        plain.create_account("Plain_Player", "pw")
        plain.login("Plain_Player", "pw")
        find = Writer().string("ATVGameSession").u32(0).u32(20).bytes()
        ok, body = plain.call(21, 17, find)
        self.assertEqual(body[:4], bytes(4))                                      # the tunnelled room is not listed
        ok, _ = plain.call(21, 11, Writer().u32(gid).string("").bytes())
        self.assertFalse(ok)
        self.assertEqual(self.net.server.counters.get("participate_refused_tunnel"), 1)
        ok, body = _b[0].call(21, 17, find)
        self.assertEqual(struct.unpack("<I", body[:4])[0], 1)                     # a tunnelled player sees it

    def test_peer_answer_only_inside_a_gathering(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby(join=False)
        self.assertEqual(self.peer(aaddr, akey, bvip), [])
        self.assertEqual(self.peer(aaddr, bkey, bvip), [])                        # wrong key
        self.assertEqual(self.peer(("stranger.test", 9), akey, bvip), [])

    def test_python_and_dll_sign_the_same_way(self):
        # test/tunneltest.cpp checks the same vector against src/tunnel_core.h
        key = bytes(range(16))
        head = b"SR\x02\x03" + bytes([10, 254, 1, 2]) + struct.pack(">HH", 9103, 9104)
        self.assertEqual(relay.mac(key, head, b"hello quazal").hex(), "017f0191645eb18f")
        self.assertEqual(hmac.new(key, head + b"hello quazal", hashlib.sha256).digest()[:8], relay.mac(key, head, b"hello quazal"))

    def test_no_address_in_the_log(self):
        with self.assertLogs("rdvd.relay", "INFO") as logs:
            (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby()
            self.send(baddr, bkey, avip, b"x")
        text = "\n".join(logs.output)
        self.assertIn("relayed", text)
        for s in ("player1", "player2", "10.254", "192.168"):
            self.assertNotIn(s, text)


if __name__ == "__main__":
    unittest.main()
