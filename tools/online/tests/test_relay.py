"""The relay port: registration, binding to a session, forwarding only inside a gathering, no reflection."""
import struct
import unittest

from support import Net
from rdvd import relay
from rdvd.codec import Writer
from rdvd.testclient import atv_session


class RelayTest(unittest.TestCase):
    def setUp(self):
        self.net = Net()
        self.addCleanup(self.net.close)
        self.r = self.net.server.relay

    def hello(self, addr, nonce=b"12345678"):
        n = len(self.net.relayed)
        self.net.server.datagram("relay", b"SR\x01" + nonce, addr, self.net.now)
        out = self.net.relayed[n:]
        if not out:
            return None
        (data, to), = out
        self.assertEqual((to, data[:3], data[3:11]), (addr, b"SR\x02", nonce))
        return data[11:15], data[15:31]          # vip, key

    def player(self, name, ip, tunnel_port=40000):
        """A signed-in client whose game registered its virtual address, as the DLL will make it do."""
        vip, key = self.hello((ip, tunnel_port))
        c = self.net.client(ip)
        c.local_url = "prudp:/address=%s;port=9103;sid=15" % relay.vip_text(vip)
        self.assertEqual(c.create_account(name, "pw"), 0x00010001)
        self.assertEqual(c.login(name, "pw"), "ok")
        return c, vip, key, (ip, tunnel_port)

    def send(self, addr, key, dst_vip, payload):
        n = len(self.net.relayed)
        self.net.server.datagram("relay", b"SR\x03" + dst_vip + relay.mac(key, b"S", dst_vip, payload) + payload, addr, self.net.now)
        return self.net.relayed[n:]

    def lobby(self):
        a, avip, akey, aaddr = self.player("Host_Player", "player1.test")
        b, bvip, bkey, baddr = self.player("Join_Player", "player2.test")
        gid = struct.unpack("<I", a.call(21, 1, atv_session())[1])[0]
        a.call(21, 11, Writer().u32(gid).string("").bytes())
        return (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid

    def test_hello_gives_a_virtual_address_and_a_key(self):
        vip, key = self.hello(("player1.test", 40000))
        self.assertEqual((vip[0], vip[1], len(key)), (10, 254, 16))
        self.assertEqual(self.hello(("player1.test", 40000)), (vip, key))            # the keep-alive: same registration
        self.assertNotEqual(self.hello(("player1.test", 40001))[0], vip)

    def test_welcome_is_not_larger_than_useful_and_only_to_the_asker(self):
        self.hello(("player1.test", 40000))
        data, to = self.net.relayed[-1]
        self.assertEqual((len(data), to), (31, ("player1.test", 40000)))

    def test_register_announces_only_the_virtual_address(self):
        c, vip, _key, _addr = self.player("Host_Player", "player1.test")
        self.assertEqual(c.public_url, "prudp:/address=%s;port=9103;sid=15;type=3" % relay.vip_text(vip))
        other = self.net.client("player2.test")
        other.create_account("Join_Player", "pw")
        other.login("Join_Player", "pw")
        ok, body = other.call(11, 3, Writer().u32(c.cid).u32(c.pid).bytes())
        self.assertNotIn(b"player1.test", body)
        self.assertIn(relay.vip_text(vip).encode(), body)

    def test_forward_inside_a_gathering_both_ways(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby()
        self.assertEqual(self.send(baddr, bkey, avip, b"early"), [])             # not a participant yet
        b.call(21, 11, Writer().u32(gid).string("").bytes())
        (data, to), = self.send(baddr, bkey, avip, b"hello host")
        self.assertEqual((to, data[:3], data[3:7], data[15:]), (aaddr, b"SR\x04", bvip, b"hello host"))
        self.assertEqual(data[7:15], relay.mac(akey, b"D", bvip, b"hello host"))  # signed for the receiver
        (data, to), = self.send(aaddr, akey, bvip, b"hello joiner")
        self.assertEqual((to, data[3:7], data[15:]), (baddr, avip, b"hello joiner"))
        self.assertEqual(self.net.server.counters["relay_datagrams"], 2)
        self.assertEqual(self.net.server.counters["relay_bytes"], len(b"hello host") + len(b"hello joiner"))

    def test_refusals(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby()
        b.call(21, 11, Writer().u32(gid).string("").bytes())
        self.assertEqual(self.send(baddr, akey, avip, b"x"), [])                 # another player's key
        self.assertEqual(self.send(("stranger.test", 1), bkey, avip, b"x"), [])       # unregistered source
        self.assertEqual(self.send(baddr, bkey, bytes([10, 254, 9, 9]), b"x"), [])   # nobody there
        self.net.server.datagram("relay", b"junk", baddr, self.net.now)
        self.net.server.datagram("relay", b"SR\x03" + avip + b"\0" * 8 + b"x" * 5000, baddr, self.net.now)
        self.assertEqual(self.net.relayed[-1][0][:3], b"SR\x02")                 # nothing was sent since the hellos
        b.call(21, 12, Writer().u32(gid).string("").bytes())                    # left the gathering
        self.assertEqual(self.send(baddr, bkey, avip, b"x"), [])

    def test_virtual_address_of_another_ip_cannot_be_claimed(self):
        vip, _key = self.hello(("player1.test", 40000))
        thief = self.net.client("thief.test")
        thief.local_url = "prudp:/address=%s;port=9103;sid=15" % relay.vip_text(vip)
        thief.create_account("Thief_Player", "pw")
        self.assertEqual(thief.login("Thief_Player", "pw"), "ok")
        self.assertEqual(thief.public_url, "prudp:/address=thief.test;port=9103;sid=15;type=3")
        self.assertIsNone(self.r.by_vip[vip].pid)

    def test_rate_limit(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby()
        b.call(21, 11, Writer().u32(gid).string("").bytes())
        out = sum(len(self.send(baddr, bkey, avip, b"p" * 100)) for _ in range(900))
        self.assertEqual(out, 600)                                               # the burst, then nothing
        self.assertEqual(self.net.server.counters["relay_limited"], 300)

    def test_hello_rate_per_address(self):
        got = [self.hello(("player1.test", 41000 + i)) for i in range(14)]
        self.assertEqual(sum(1 for g in got if g), 10)

    def test_registration_expires_and_session_end_releases_it(self):
        (a, avip, akey, aaddr), (b, bvip, bkey, baddr), gid = self.lobby()
        b.links["secure"].disconnect()
        self.assertIsNone(self.r.by_vip[bvip].pid)
        self.net.advance(relay.REGISTRATION_LIFETIME + 61)
        self.assertNotIn(bvip, self.r.by_vip)


if __name__ == "__main__":
    unittest.main()
