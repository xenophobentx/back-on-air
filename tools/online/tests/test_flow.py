"""The login flow and the matchmaking calls, end to end in one process: replayed captures and the test client."""
import os
import struct
import unittest
from unittest import mock

from support import Net
from rdvd import codec, kerberos, transport
from rdvd.codec import Packet, Reader, Writer
from rdvd.testclient import atv_session
from test_codec import BASE, SYN, CONNECT, LOGIN, LOGIN_GUEST, REQUEST_TICKET, PASSWORD, KEY_HEX, NAME


def answer_of(dgrams):
    """The RMC answer among the datagrams the server sent back -> (ok, method or code, body)."""
    for d in dgrams:
        for p in codec.decode_packets(d, BASE):
            if p.type == codec.DATA and not p.flags & codec.F_ACK:
                _proto, ok, _call, what, body = codec.decode_response(codec.unwrap_payload(p.payload))
                return ok, what, body
    return None


class Base(unittest.TestCase):
    def setUp(self):
        self.net = Net()
        self.addCleanup(self.net.close)

    def signed_up(self, name="Racer_One", password="pw-one", ip="player1.test"):
        c = self.net.client(ip)
        self.assertEqual(c.create_account(name, password), codec.OK)
        self.assertEqual(c.login(name, password), "ok")
        return c


class ReplayCaptures(Base):
    """The exact datagrams the game sent, fed to the server. Our connection signature is forced to the stub's."""

    def replay_handshake(self, sig, connect):
        with mock.patch.object(transport.os, "urandom", lambda n: struct.pack("<I", sig)):
            (syn_ack,) = self.net.raw("auth", SYN)
        (p,) = codec.decode_packets(syn_ack, BASE)
        self.assertEqual((p.type, p.flags, p.src, p.dst, p.connsig), (codec.SYN, codec.F_ACK, 0x31, 0x3F, sig))
        (ack,) = self.net.raw("auth", connect)
        (p,) = codec.decode_packets(ack, BASE)
        self.assertEqual((p.type, p.flags), (codec.CONNECT, codec.F_ACK))
        return p

    def test_syn_ack_is_what_the_game_accepted(self):
        with mock.patch.object(transport.os, "urandom", lambda n: struct.pack("<I", 0x51A70001)):
            (syn_ack,) = self.net.raw("auth", SYN)
        self.assertEqual(syn_ack.hex(), "313f08000000000000000100a75143")       # byte for byte the stub's answer

    def test_connect_ack_is_what_the_game_accepted(self):
        p = self.replay_handshake(0x51A70001, CONNECT)
        self.assertEqual(p.encode(BASE).hex(), "313f09012879b01401000100a751ab")

    def test_login_of_an_unknown_name(self):
        self.replay_handshake(0x51A70001, CONNECT)
        out = self.net.raw("auth", LOGIN)
        (ack,) = codec.decode_packets(out[0], BASE)
        self.assertEqual(out[0].hex(), "313f0a012879b014020000b4")                  # the DATA acknowledgement
        self.assertEqual((ack.flags, ack.seq), (codec.F_ACK, 2))
        ok, method, body = answer_of(out)
        r = Reader(body)
        self.assertEqual((ok, method), (True, 1))
        self.assertEqual((r.u32(), r.u32(), r.buffer(), r.string()), (codec.ERR_INVALID_USERNAME, 0, b"", "prudp:/"))

    def test_login_of_a_known_account(self):
        pid = self.net.server.accounts.create(NAME, bytes.fromhex(KEY_HEX))
        self.replay_handshake(0x51A70001, CONNECT)
        ok, _method, body = answer_of(self.net.raw("auth", LOGIN))
        r = Reader(body)
        self.assertEqual((ok, r.u32(), r.u32()), (True, codec.OK, pid))
        # The game would open the ticket with the key it derives from the typed password.
        key = kerberos.principal_key(pid, kerberos.password_hash(PASSWORD))
        _session_key, server_pid, _internal = kerberos.open_ticket(key, r.buffer())
        self.assertEqual(server_pid, 2)
        self.assertEqual(r.string(), "prudps:/address=rdv.test;port=31121;CID=1;PID=2;sid=1;stream=3;type=2")

    def test_guest_login_and_request_ticket(self):
        connect = Packet(codec.CONNECT, codec.F_RELIABLE | codec.F_NEED_ACK, src=0x3F, dst=0x31, session=149,
                         sig=0x51A70002, seq=1, connsig=0xBADE7C52).encode(BASE)
        self.replay_handshake(0x51A70002, connect)
        ok, _m, body = answer_of(self.net.raw("auth", LOGIN_GUEST))
        r = Reader(body)
        self.assertEqual((ok, r.u32(), r.u32()), (True, codec.OK, 100))
        guest_key = kerberos.principal_key(100, kerberos.password_hash("h7fyctiuucf"))
        kerberos.open_ticket(guest_key, r.buffer())
        ok, method, body = answer_of(self.net.raw("auth", REQUEST_TICKET))
        r = Reader(body)
        self.assertEqual((ok, method, r.u32()), (True, 3, codec.OK))
        session_key, _pid, internal = kerberos.open_ticket(guest_key, r.buffer())
        self.assertEqual(kerberos.open_internal_ticket(self.net.server.secret, internal), (100, session_key))

    def test_repeated_datagram_is_acknowledged_but_not_run_twice(self):
        self.replay_handshake(0x51A70001, CONNECT)
        first = self.net.raw("auth", LOGIN)
        again = self.net.raw("auth", LOGIN)
        self.assertEqual(len(first), 2)
        self.assertEqual(again, [first[0]])


class LoginFlow(Base):
    def test_unknown_then_create_then_login(self):
        c = self.net.client()
        self.assertEqual(c.login("Racer_One", "pw-one"), "retval 0x80030064")
        self.assertEqual(c.create_account("Racer_One", "pw-one"), codec.OK)
        self.assertEqual(c.login("Racer_One", "pw-one"), "ok")
        self.assertEqual(c.pid, 1000)
        self.assertEqual(c.public_url, "prudp:/address=player1.test;port=9103;sid=15;type=3")
        self.assertIn(1000, self.net.server.sessions)

    def test_wrong_password_cannot_open_the_ticket(self):
        self.signed_up()
        self.assertEqual(self.net.client("player2.test").login("Racer_One", "guess"), "ticket does not open")

    def test_names(self):
        c = self.net.client()
        self.assertEqual(c.create_account("Racer_One", "a"), codec.OK)
        self.assertEqual(c.create_account("racer_one", "b"), codec.ERR_USERNAME_EXISTS)      # case-insensitive
        for bad in ("ab", "x" * 17, "guest", "Admin", "bad\x01name", " lead", "semi;colon"):
            self.assertEqual(self.net.client("extra%d.test" % (len(self.net.clients) + 1)).create_account(bad, "p"),
                             codec.ERR_INVALID_USERNAME, bad)
        self.assertEqual(self.net.client("player3.test").login("RACER_ONE", "a"), "ok")          # login ignores case too

    def test_automatic_name_renamed_with_the_same_key(self):
        # SSPC-25: "<Steam name>-<tag>"; the player renames himself on Steam: same key, same tag -> same account
        c = self.net.client()
        self.assertEqual(c.create_account("Old Name-7QK", "auto-secret"), codec.OK)
        self.assertEqual(self.net.client("player2.test").create_account("New Name-7QK", "auto-secret"), codec.OK)
        self.assertEqual(self.net.server.accounts.count(), 1)
        self.assertEqual(self.net.server.accounts.name_of(1000), "New Name-7QK")
        c3 = self.net.client("player3.test")
        self.assertEqual(c3.login("New Name-7QK", "auto-secret"), "ok")
        self.assertEqual(c3.pid, 1000)
        self.assertEqual(self.net.client("player4.test").login("Old Name-7QK", "auto-secret"), "retval 0x80030064")

    def test_automatic_name_other_key_or_tag_makes_a_new_account(self):
        c = self.net.client()
        self.assertEqual(c.create_account("Racer-7QK", "key-one"), codec.OK)
        self.assertEqual(self.net.client("player2.test").create_account("Racer-8QK", "key-one"), codec.OK)  # other tag
        self.assertEqual(self.net.client("player3.test").create_account("Other-7QK", "key-two"), codec.OK)  # other key
        self.assertEqual(self.net.client("player4.test").create_account("Racer-7QK", "key-two"), codec.ERR_USERNAME_EXISTS)
        self.assertEqual(self.net.server.accounts.count(), 3)
        # a typed password under plain names never renames anything
        self.assertEqual(self.net.client("player5.test").create_account("Plain", "same"), codec.OK)
        self.assertEqual(self.net.client("player6.test").create_account("Plainer", "same"), codec.OK)
        self.assertEqual(self.net.server.accounts.count(), 5)

    def test_automatic_rename_onto_a_taken_name_is_refused(self):
        self.assertEqual(self.net.client().create_account("Taken-7QK", "k1"), codec.OK)
        self.assertEqual(self.net.client("player2.test").create_account("Mine-ABC", "k2"), codec.OK)
        self.assertEqual(self.net.client("player3.test").create_account("Taken-ABC", "k2"), codec.OK)  # free name: renamed
        self.assertEqual(self.net.server.accounts.name_of(1001), "Taken-ABC")
        self.assertEqual(self.net.client("player4.test").create_account("taken-7qk", "k2"), codec.ERR_USERNAME_EXISTS)

    def test_database_holds_no_key_in_clear(self):
        self.signed_up("Racer_One", "pw-one")
        h = kerberos.password_hash("pw-one")
        raw = b"".join(bytes(v) if isinstance(v, bytes) else str(v).encode()
                       for row in self.net.server.accounts.db.execute("SELECT * FROM accounts") for v in row)
        self.assertNotIn(h, raw)
        self.assertNotIn(h.hex().encode(), raw)
        self.assertEqual(self.net.server.accounts.lookup("racer_one")[2], h)

    def test_guest_can_only_create_accounts(self):
        c = self.net.client()
        self.assertEqual(c.login("guest", "h7fyctiuucf"), "ok")
        self.assertEqual(c.call(20, 10, b"\x02\x01"), (False, codec.ERR_ACCESS_DENIED))
        self.assertEqual(c.call(21, 17, Writer().string("ATVGameSession").u32(0).u32(10).bytes()), (False, codec.ERR_ACCESS_DENIED))

    def test_only_guest_creates_accounts(self):
        c = self.signed_up()
        a = c.call(25, 1, Writer().string("Another").string("0" * 32).u32(0x20).string("x").bytes())
        self.assertEqual(a, (False, codec.ERR_ACCESS_DENIED))

    def test_request_ticket_only_for_the_logged_in_principal(self):
        self.signed_up()
        c = self.net.client("player2.test")
        link = c.open("auth")
        link.connect()
        self.assertEqual(link.call(10, 3, struct.pack("<II", 1000, 2)), (False, codec.ERR_ACCESS_DENIED))

    def test_secure_port_refuses_a_forged_ticket(self):
        c = self.net.client()
        link = c.open("secure")
        fake = kerberos.make_internal_ticket(b"X" * 32, 1000, b"k" * 16)
        reply = link.connect(Writer().buffer(fake).buffer(kerberos.make_connect_check(b"k" * 16, 1000, 0, 7)).bytes())
        self.assertIsNone(reply)
        self.assertEqual(link.call(20, 10, b"\x02\x01"), None)

    def test_calls_before_login_are_refused_on_the_secure_port(self):
        c = self.net.client()
        link = c.open("secure")
        self.assertIsNone(link.connect(b""))

    def test_second_login_replaces_the_first(self):
        a = self.signed_up()
        b = self.net.client("player2.test")
        self.assertEqual(b.login("Racer_One", "pw-one"), "ok")
        self.assertIs(self.net.server.sessions[1000].addr, list(self.net.clients)[1])
        self.assertIsNone(a.call(20, 10, b"\x02\x01"))

    def test_friends_poll_gets_an_empty_list(self):
        c = self.signed_up()
        self.assertEqual(c.call(20, 10, b"\x02\x01"), (True, struct.pack("<I", 0)))

    def test_unknown_call_is_answered_not_ignored(self):
        c = self.signed_up()
        self.assertEqual(c.call(101, 2, b"whatever"), (False, codec.ERR_NOT_IMPLEMENTED))

    def test_malformed_parameters(self):
        c = self.signed_up()
        self.assertEqual(c.call(21, 17, b"\xff\xff"), (False, codec.ERR_INVALID_ARGUMENT))

    def test_change_password(self):
        c = self.signed_up()
        self.assertEqual(c.call(25, 4, Writer().string(kerberos.password_hash("new").hex()).bytes()), (True, b"\x01"))
        self.assertEqual(self.net.client("player2.test").login("Racer_One", "pw-one"), "ticket does not open")
        self.assertEqual(self.net.client("player3.test").login("Racer_One", "new"), "ok")

    def test_disconnect_ends_the_session(self):
        c = self.signed_up()
        c.links["secure"].disconnect()
        self.assertTrue(c.links["secure"].closed)
        self.assertNotIn(1000, self.net.server.sessions)


class Limits(Base):
    def test_login_rate_per_address(self):
        c = self.net.client()
        results = []
        for i in range(14):
            results.append(c.login("nobody%d" % i, "x"))
            self.net.advance(transport.AUTH_IDLE_DROP + 1 if i == 6 else 0.5)     # the failed logins' connections expire
        self.assertEqual(results[0], "retval 0x80030064")
        self.assertTrue(self.net.server.counters.get("login_limited"))
        self.assertIn("login call failed: (False, %d)" % codec.ERR_ACCESS_DENIED, results)

    def test_account_creation_rate(self):
        got = []
        for i in range(7):
            got.append(self.net.client("player1.test").create_account("Name%d" % i, "x"))
            self.net.advance(3)                      # the guest logins stay inside the per-address login limit
        self.assertEqual(got[:5], [codec.OK] * 5)
        self.assertEqual(got[5], codec.ERR_ACCESS_DENIED)
        self.assertEqual(self.net.server.accounts.count(), 5)
        self.assertEqual(self.net.client("player2.test").create_account("Other", "x"), codec.OK)   # per address

    def test_launch_night_many_new_players(self):
        got = [self.net.client("player%d.test" % i).create_account("Newcomer%d" % i, "x") for i in range(60)]
        self.assertEqual(got, [codec.OK] * 60)

    def test_global_creation_ceiling(self):
        self.net.server.limits.create_global.t["all"] = (0.0, self.net.now)    # as after a flood from many addresses
        self.assertEqual(self.net.client("player1.test").create_account("Late", "x"), codec.ERR_ACCESS_DENIED)

    def test_taken_name_costs_no_creation_token(self):
        self.assertEqual(self.net.client("player1.test").create_account("Taken", "x"), codec.OK)
        for _ in range(6):
            self.assertEqual(self.net.client("player2.test").create_account("taken", "y"), codec.ERR_USERNAME_EXISTS)
            self.net.advance(3)
        self.assertEqual(self.net.client("player2.test").create_account("Fresh", "y"), codec.OK)

    def test_draining_a_name_from_elsewhere_does_not_lock_its_owner_out(self):
        self.assertEqual(self.net.client("owner.test").create_account("Racer_One", "pw-one"), codec.OK)
        attacker = self.net.client("attacker.test")
        got = [attacker.login("Racer_One", "guess%d" % i) for i in range(7)]   # Login(name) from one address
        self.assertEqual(got[-1], "login call failed: (False, %d)" % codec.ERR_ACCESS_DENIED)
        self.assertEqual(self.net.client("owner.test", 9104).login("Racer_One", "pw-one"), "ok")

    def test_connections_per_address(self):
        c = self.net.client()
        for _ in range(self.net.server.limits.max_per_address):
            c.open("auth").connect()
        with self.assertRaises(AssertionError):
            c.open("auth").connect()

    def test_half_open_connections_per_address(self):
        for port in range(9100, 9110):
            self.net.raw("auth", SYN, ("player9.test", port))
        self.assertEqual(len(self.net.server.auth.conns), self.net.server.limits.max_half_open_per_address)

    def test_forged_syns_evict_the_oldest_half_open_not_players(self):
        lim = self.net.server.limits
        c = self.signed_up()
        before = len(self.net.server.secure.conns)
        for i in range(lim.max_connections + 50):      # one SYN each from many forged addresses
            self.net.raw("secure", SYN, ("forged%d.test" % i, 4000))
        ep = self.net.server.secure
        self.assertLessEqual(len(ep.half_open), lim.max_half_open)
        self.assertEqual(len(ep.conns) - len(ep.half_open), before)    # the player's connection is still there
        self.assertIn(1000, self.net.server.sessions)
        self.assertTrue(self.net.server.counters.get("half_open_evicted"))
        self.assertEqual(c.call(20, 10, b"\x02\x01"), (True, struct.pack("<I", 0)))
        self.assertEqual(self.net.client("player2.test").create_account("Second", "x"), codec.OK)   # a new one gets in

    def test_full_rate_table_forgets_the_oldest_key(self):
        from rdvd.limits import Buckets
        b = Buckets(1.0, 2.0, max_keys=3)
        for k in "abc":
            b.allow(k, 0.0)
        b.allow("a", 0.0)                             # used again: "b" is now the oldest
        self.assertTrue(b.allow("d", 0.0))            # a new key is answered, not refused
        self.assertEqual(list(b.t), ["c", "a", "d"])
        self.assertFalse(b.allow("a", 0.0))           # "a" kept its spent tokens

    def test_half_open_connections_expire(self):
        self.net.raw("auth", SYN)
        self.assertEqual(len(self.net.server.auth.conns), 1)
        self.net.advance(transport.HANDSHAKE_TIMEOUT + 1)
        self.assertEqual(len(self.net.server.auth.conns), 0)

    def test_syn_answer_is_not_larger_than_the_syn(self):
        (out,) = self.net.raw("auth", SYN)
        self.assertLessEqual(len(out), len(SYN))

    def test_packet_from_another_address_is_ignored(self):
        c = self.signed_up()
        link = c.links["secure"]
        forged = Packet(codec.DISCONNECT, codec.F_RELIABLE | codec.F_NEED_ACK, src=0x3F, dst=0x31, session=link.session,
                        sig=link.server_sig, seq=link.seq + 1).encode(BASE)
        self.assertEqual(self.net.raw("secure", forged, ("stranger.test", 9103)), [])
        self.assertIn(1000, self.net.server.sessions)


class Reliability(Base):
    def test_unacknowledged_answer_is_resent_then_the_connection_dropped(self):
        c = self.signed_up()
        self.net.drop_to_client = True
        n = len(self.net.sent["secure"])
        c.call(20, 10, b"\x02\x01")
        self.net.advance(transport.RESEND_AFTER * 3 + 0.5)
        resent = [d for d, _a in self.net.sent["secure"][n:]]
        self.assertGreaterEqual(resent.count(resent[1]), 3)
        self.net.advance(transport.RESEND_AFTER * transport.RESEND_TRIES + 2)
        self.assertNotIn(1000, self.net.server.sessions)

    def pings_sent(self, n):
        return [p.type for d, _a in self.net.sent["secure"][n:] for p in codec.decode_packets(d, BASE)].count(codec.PING)

    def test_silent_client_is_pinged_from_the_tenth_second(self):
        self.signed_up()
        n = len(self.net.sent["secure"])
        self.net.advance(10.5)
        self.assertEqual(self.pings_sent(n), 1)       # before a home router's ~30 s UDP mapping time-out
        self.net.advance(20)
        self.assertEqual(self.pings_sent(n), 3)

    def test_client_that_answers_pings_is_kept(self):
        c = self.signed_up()
        link = c.links["secure"]
        addr = list(self.net.clients)[0]
        for _ in range(int(transport.IDLE_DROP_IN_GAME / transport.PING_EVERY) + 5):
            self.net.advance(transport.PING_EVERY, step=1)
            for p in [p for p in link.raw if p.type == codec.PING and not p.flags & codec.F_ACK]:
                self.net.server.datagram("secure", Packet(codec.PING, codec.F_ACK, src=0x3F, dst=0x31, session=link.session,
                                                          sig=link.server_sig, seq=p.seq).encode(BASE), addr, self.net.now)
            link.raw.clear()
        self.assertIn(1000, self.net.server.sessions)

    def test_silent_player_in_a_room_with_others_is_kept_through_a_race(self):
        host = self.signed_up("Host_Player", "h", "player1.test")
        guest = self.signed_up("Join_Player", "j", "player2.test")
        ok, body = host.call(21, 1, atv_session())
        gid = struct.unpack("<I", body)[0]
        host.call(21, 11, Writer().u32(gid).string("").bytes())
        guest.call(21, 11, Writer().u32(gid).string("").bytes())
        self.net.advance(transport.IDLE_DROP + 120, step=1)    # a race with loading and results: no call, no answer
        self.assertIn(host.pid, self.net.server.sessions)
        self.assertIn(guest.pid, self.net.server.sessions)
        self.assertIn(gid, self.net.server.gatherings)
        self.net.advance(transport.IDLE_DROP_IN_GAME - transport.IDLE_DROP, step=1)
        self.assertEqual(self.net.server.sessions, {})  # a crashed or gone game still goes
        self.assertNotIn(gid, self.net.server.gatherings)

    def test_silent_player_alone_in_a_room_goes_after_three_minutes(self):
        host = self.signed_up("Host_Player", "h", "player1.test")
        ok, body = host.call(21, 1, atv_session())
        host.call(21, 11, Writer().u32(struct.unpack("<I", body)[0]).string("").bytes())
        self.net.advance(transport.IDLE_DROP + 1)
        self.assertNotIn(host.pid, self.net.server.sessions)

    def test_new_port_on_the_same_address_is_followed(self):
        c = self.signed_up()
        link = c.links["secure"]
        self.net.clients[("player1.test", 40000)] = c                   # the router mapped the client anew
        self.net.server.datagram("secure", Packet(codec.PING, codec.F_NEED_ACK, src=0x3F, dst=0x31, session=link.session,
                                                  sig=link.server_sig, seq=7).encode(BASE), ("player1.test", 40000), self.net.now)
        s = self.net.server.sessions[1000]
        self.assertEqual(s.addr, ("player1.test", 40000))
        self.assertEqual(self.net.sent["secure"][-1][1], ("player1.test", 40000))
        self.assertEqual(self.net.server.counters.get("port_changed"), 1)

    def test_silent_client_is_pinged_then_dropped(self):
        self.signed_up()
        n = len(self.net.sent["secure"])
        self.net.advance(transport.IDLE_PING + 1)
        types = [p.type for d, _a in self.net.sent["secure"][n:] for p in codec.decode_packets(d, BASE)]
        self.assertEqual(types, [codec.PING])
        n = len(self.net.sent["secure"])
        self.net.advance(transport.PING_EVERY * 3)
        types = [p.type for d, _a in self.net.sent["secure"][n:] for p in codec.decode_packets(d, BASE)]
        self.assertEqual(types, [codec.PING] * 3)                       # pinged again while it stays silent
        self.assertIn(1000, self.net.server.sessions)
        self.net.advance(transport.IDLE_DROP)
        self.assertNotIn(1000, self.net.server.sessions)

    def test_out_of_order_calls_run_in_order(self):
        c = self.signed_up()
        link = c.links["secure"]

        def data(seq, call):
            return Packet(codec.DATA, codec.F_RELIABLE | codec.F_NEED_ACK, src=0x3F, dst=0x31, session=link.session,
                          sig=link.server_sig, seq=seq, payload=codec.wrap_payload(codec.encode_request(20, call, 10, b"\x02\x01"))).encode(BASE)
        addr = list(self.net.clients)[0]
        self.net.server.datagram("secure", data(link.seq + 2, 902), addr, self.net.now)
        self.assertNotIn(902, link.answers)
        self.net.server.datagram("secure", data(link.seq + 1, 901), addr, self.net.now)
        self.assertEqual(list(link.answers), [901, 902])

    def test_large_answer_is_fragmented_and_reassembled(self):
        c = self.signed_up()
        s = self.net.server.sessions[1000]
        s.call(14, 1, b"z" * 2500)
        self.assertEqual(c.links["secure"].pushed, [(14, 1, b"z" * 2500)])


class MatchMaking(Base):
    def setUp(self):
        Base.setUp(self)
        self.host = self.signed_up("Host_Player", "h", "player1.test")
        self.guest = self.signed_up("Join_Player", "j", "player2.test")

    def register(self, c, **kw):
        ok, body = c.call(21, 1, atv_session(**kw))
        self.assertTrue(ok)
        return struct.unpack("<I", body)[0]

    def find(self, c, kind="ATVGameSession"):
        ok, body = c.call(21, 17, Writer().string(kind).u32(0).u32(50).bytes())
        self.assertTrue(ok)
        out = []
        r = Reader(body)
        for _ in range(r.u32()):
            name, g = r.anydata()
            gr = Reader(g)
            out.append({"type": name, "gid": gr.u32(), "owner": gr.u32(), "host": gr.u32(), "min": gr.u16(), "max": gr.u16(),
                        "policy": gr.u32(), "arg": gr.u32(), "flags": gr.u32(), "state": gr.u32(), "desc": gr.string(),
                        "attrs": struct.unpack("<10I", gr.rest())})
        return out

    def test_host_registers_and_another_player_finds_it(self):
        gid = self.register(self.host, description="room", attributes=tuple(range(1, 11)))
        (g,) = self.find(self.guest)
        self.assertEqual((g["type"], g["gid"], g["owner"], g["host"], g["max"], g["desc"]),
                         ("ATVGameSession", gid, self.host.pid, self.host.pid, 8, "room"))
        self.assertEqual(g["attrs"], tuple(range(1, 11)))
        self.assertEqual(self.find(self.guest, "OtherType"), [])

    def test_join_urls_and_leave(self):
        gid = self.register(self.host)
        self.assertEqual(self.host.call(21, 11, Writer().u32(gid).string("").bytes()), (True, b"\x01"))
        self.assertEqual(self.guest.call(21, 11, Writer().u32(gid).string("hi").bytes()), (True, b"\x01"))
        ok, body = self.guest.call(21, 16, Writer().u32(gid).bytes())
        urls = Reader(body).list(lambda r: r.string())
        tail = ";PID=%d;RVCID=%d" % (self.host.pid, self.host.cid)
        own = ";PID=%d;RVCID=%d" % (self.guest.pid, self.guest.cid)
        # The host's stations first, then the asker's own (the game stops at its own PID and aborts without it).
        self.assertEqual(urls[:2], ["prudp:/address=player1.test;port=9103;sid=15;type=3" + tail, self.host.local_url + tail])
        self.assertTrue(urls[2:] and all(u.endswith(own) for u in urls[2:]))
        # The host asking sees only itself (nobody joined before it), so the game's walk ends at once.
        ok, body = self.host.call(21, 16, Writer().u32(gid).bytes())
        self.assertTrue(all(u.endswith(tail) for u in Reader(body).list(lambda r: r.string())))
        self.assertEqual(self.guest.call(21, 12, Writer().u32(gid).string("").bytes()), (True, b"\x01"))
        self.assertEqual(self.net.server.gatherings[gid].participants, [self.host.pid])

    def test_join_urls_end_with_the_askers_own_even_before_participate(self):
        gid = self.register(self.host)
        self.host.call(21, 11, Writer().u32(gid).string("").bytes())
        ok, body = self.guest.call(21, 16, Writer().u32(gid).bytes())
        urls = Reader(body).list(lambda r: r.string())
        self.assertTrue(urls[-1].endswith(";PID=%d;RVCID=%d" % (self.guest.pid, self.guest.cid)))
        self.assertTrue(urls[0].endswith(";PID=%d;RVCID=%d" % (self.host.pid, self.host.cid)))

    def test_a_joiners_update_is_accepted_but_not_applied(self):
        gid = self.register(self.host, description="room")
        self.host.call(21, 11, Writer().u32(gid).string("").bytes())
        self.guest.call(21, 11, Writer().u32(gid).string("").bytes())
        self.assertEqual(self.guest.call(21, 4, atv_session(description="hijack", gid=gid)), (True, b"\x01"))
        self.assertEqual(self.net.server.gatherings[gid].description, "room")

    def test_request_urls_of_a_player(self):
        ok, body = self.guest.call(11, 3, Writer().u32(self.host.cid).u32(self.host.pid).bytes())
        r = Reader(body)
        tail = ";PID=%d;RVCID=%d" % (self.host.pid, self.host.cid)
        self.assertEqual((r.bool(), r.list(lambda x: x.string())),
                         (True, ["prudp:/address=player1.test;port=9103;sid=15;type=3" + tail, self.host.local_url + tail]))
        ok, body = self.guest.call(11, 3, Writer().u32(1).u32(4242).bytes())
        self.assertEqual(body, b"\x00" + struct.pack("<I", 0))

    def test_full_room(self):
        gid = self.register(self.host, max_players=1)
        self.host.call(21, 11, Writer().u32(gid).string("").bytes())
        self.assertEqual(self.guest.call(21, 11, Writer().u32(gid).string("").bytes()), (False, codec.ERR_SESSION_FULL))

    def test_only_the_owner_changes_or_removes_a_room(self):
        gid = self.register(self.host, description="a")
        self.assertEqual(self.guest.call(21, 4, atv_session(description="hacked", gid=gid)), (True, b"\x00"))
        self.assertEqual(self.guest.call(21, 2, Writer().u32(gid).bytes()), (True, b"\x00"))
        self.assertEqual(self.guest.call(21, 30, Writer().u32(gid).u32(5).bytes()), (True, b"\x00"))
        self.assertEqual(self.host.call(21, 4, atv_session(description="b", max_players=4, gid=gid)), (True, b"\x01"))
        (g,) = self.find(self.guest)
        self.assertEqual((g["desc"], g["max"], g["owner"]), ("b", 4, self.host.pid))
        self.assertEqual(self.host.call(21, 2, Writer().u32(gid).bytes()), (True, b"\x01"))
        self.assertEqual(self.find(self.guest), [])

    def test_find_by_single_id_and_launch(self):
        gid = self.register(self.host)
        ok, body = self.guest.call(21, 21, Writer().u32(gid).bytes())
        self.assertEqual((ok, body[0]), (True, 1))
        self.assertEqual(self.guest.call(21, 21, Writer().u32(gid + 5).bytes()), (False, codec.ERR_INVALID_GID))
        self.assertEqual(self.host.call(21, 26, Writer().u32(gid).string("prudp:/address=player1.test;port=9103").bytes()), (True, b"\x01"))

    def test_room_goes_when_the_host_goes(self):
        gid = self.register(self.host)
        self.guest.call(21, 11, Writer().u32(gid).string("").bytes())
        self.host.links["secure"].disconnect()
        self.assertEqual(self.find(self.guest), [])

    def test_probe_request_reaches_the_target(self):
        self.assertEqual(self.guest.call(3, 1, Writer().list([self.host.public_url], lambda w, u: w.string(u)).bytes()), (True, b""))
        self.assertEqual(self.host.links["secure"].pushed, [(3, 2, Writer().string(self.guest.public_url + ";PID=%d;RVCID=%d" % (self.guest.pid, self.guest.cid)).bytes())])

    def test_notifications_when_enabled(self):
        self.net.server.notifications = True
        gid = self.register(self.host)
        self.guest.call(21, 11, Writer().u32(gid).string("hi").bytes())
        (proto, method, params) = self.host.links["secure"].pushed[-1]
        r = Reader(params)
        self.assertEqual((proto, method, r.u32(), r.u32(), r.u32(), r.u32(), r.string()),
                         (14, 1, self.guest.pid, 3001, gid, self.guest.pid, "hi"))


if __name__ == "__main__":
    unittest.main()
