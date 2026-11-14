"""Statistics and the web files (stats.py, geoip.py): counts per UTC day, rollover, restart, privacy of the files."""
import json
import os
import stat
import tempfile
import time
import unittest

from support import Net
from rdvd import codec
from rdvd.codec import Writer
from rdvd.geoip import GeoIP
from rdvd.stats import Stats, read_token, utc_date
from rdvd.testclient import atv_session

TOKEN = "owner-token-0123456789abcdef"
PUBLIC_ROOM = (1, 0, 0, 0, 0, 0, 1, 0, 0, 0)      # captured: PUBLIC GAME -> RACE
PRIVATE_ROOM = (2, 0, 0, 0, 0, 0, 1, 0, 0, 0)


class Clock:
    def __init__(self):
        self.t = float(int(time.time()) // 86400 * 86400 + 12 * 3600)    # today, 12:00 UTC (the accounts use time.time())

    def __call__(self):
        return self.t


class StatsBase(unittest.TestCase):
    race_detect = False

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = self.tmp.name
        csv = os.path.join(self.dir, "geo.csv")
        # The country lookup needs numeric addresses: these tests use the documentation ranges of RFC 5737
        # (192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24), which belong to nobody. Other tests use host names.
        with open(csv, "w") as f:
            f.write("192.0.2.0,192.0.2.255,HU\n198.51.100.0,198.51.100.255,DE\n::,::ffff,ZZ\n")
        self.geo = GeoIP(csv)
        self.clock = Clock()
        self.stats = self.make_stats()
        self.net = Net(stats=self.stats)
        self.addCleanup(self.net.close)

    def make_stats(self):
        return Stats(os.path.join(self.dir, "state"), os.path.join(self.dir, "public"), TOKEN, self.geo,
                     self.race_detect, "https://example.org/online", self.clock)

    @property
    def day(self):
        return self.stats.day

    def player(self, name, ip="192.0.2.1", earlier_account=False):
        c = self.net.client(ip)
        self.assertEqual(c.create_account(name, "pw"), codec.OK)
        if earlier_account:
            db = self.net.server.accounts.db
            db.execute("UPDATE accounts SET created = created - 3 WHERE lname = ?", (name.lower(),))
            db.commit()
        self.assertEqual(c.login(name, "pw"), "ok")
        return c

    def room(self, c, attributes=PUBLIC_ROOM, description="Host_Name-ABC"):
        ok, body = c.call(21, 1, atv_session(description=description, attributes=attributes))
        self.assertTrue(ok)
        gid = int.from_bytes(body[:4], "little")
        self.join(c, gid)                 # the game's host takes part in its own room right after (plan 2.5)
        return gid

    def join(self, c, gid):
        ok, _ = c.call(21, 11, Writer().u32(gid).string("").bytes())
        self.assertTrue(ok)

    def read(self, *parts):
        with open(os.path.join(self.dir, *parts), encoding="utf-8") as f:
            return f.read()


class Players(StatsBase):
    def test_signins_unique_returning_peak_and_countries(self):
        a = self.player("Old_Timer", "192.0.2.1", earlier_account=True)
        self.player("New_Comer", "198.51.100.7")
        a.links["secure"].disconnect()
        self.assertEqual(a.login("Old_Timer", "pw"), "ok")
        d = self.day
        self.assertEqual((d["signins"], d["unique_players"], d["returning_players"]), (3, 2, 1))
        self.assertEqual(d["countries"], {"HU": [1, 2], "DE": [1, 1]})
        self.assertEqual(d["peak_players"], 2)
        self.assertEqual(d["hour_peak"][12], 2)

    def test_guest_login_is_not_a_player(self):
        c = self.net.client()
        c.create_account("Some_One", "pw")      # goes through the guest login
        self.assertEqual(self.day["signins"], 0)

    def test_unknown_address_and_no_database(self):
        self.assertEqual(self.geo.country("203.0.113.1"), "??")
        self.assertEqual(self.geo.country("not an address"), "??")
        self.assertEqual(len(self.geo), 2)                     # the IPv6 and ZZ line are left out


class Games(StatsBase):
    def setUp(self):
        StatsBase.setUp(self)
        self.host = self.player("Host_Name-ABC", "192.0.2.1")
        self.other = self.player("Join_Name-XYZ", "192.0.2.2")

    def test_created_public_private_and_event(self):
        self.room(self.host)
        self.room(self.other, PRIVATE_ROOM)
        d = self.day
        self.assertEqual((d["games_created"], d["games_public"], d["games_private"], d["created_event_0"]), (3 - 1, 1, 1, 2))
        n = self.stats.now_counts()
        self.assertEqual((n["games"], n["games_private"], n["games_waiting"], n["games_racing"]), (2, 1, 1, None))

    def test_open_games_are_neutral_rows_of_public_rooms(self):
        gid = self.room(self.host)
        self.join(self.other, gid)
        self.room(self.player("Third_One", "192.0.2.3"), PRIVATE_ROOM)
        self.assertEqual(self.stats.games_list(), [{"mode": "", "players": 2, "max": 8, "state": "waiting"}])

    def test_room_closed_alone_and_host_left(self):
        gid = self.room(self.host)
        self.clock.t += 90
        ok, _ = self.host.call(21, 2, Writer().u32(gid).bytes())
        self.assertTrue(ok)
        gid = self.room(self.host)
        self.join(self.other, gid)
        self.host.links["secure"].disconnect()
        d = self.day
        self.assertEqual((d["closed_alone"], d["closed_alone_never"], d["closed_alone_secs"], d["closed_alone_lt5m"]), (1, 1, 90, 1))
        self.assertEqual((d["host_left"], d["host_left_players"], d["host_left_lost"]), (1, 1, 1))

    def test_another_room_by_the_same_host_is_a_move(self):
        gid = self.room(self.host)
        self.join(self.other, gid)
        self.room(self.host)
        self.assertEqual((self.day["host_left"], self.day["host_left_moved"]), (1, 1))

    def test_no_race_counted_without_race_detect(self):
        gid = self.room(self.host)
        self.join(self.other, gid)
        self.net.advance(15)
        self.assertEqual(self.day["games_started"], 0)
        self.assertIsNone(self.stats.public_json()["today"]["games_started"])


class Races(StatsBase):
    race_detect = True

    def poll(self, c, seconds):
        """The menus' Friends poll (plan 2.5), once a second."""
        for _ in range(int(seconds)):
            c.call(20, 10, b"\x00")
            self.net.advance(1)

    def test_a_quiet_host_with_two_players_is_racing(self):
        host = self.player("Host_Name-ABC", "192.0.2.1")
        other = self.player("Join_Name-XYZ", "192.0.2.2")
        gid = self.room(host)
        self.net.advance(15)
        self.assertEqual(self.day["games_started"], 0)        # alone and quiet: never a race
        self.poll(host, 1)
        self.join(other, gid)
        self.poll(host, 4)
        self.assertEqual(self.day["games_started"], 0)        # the host polls: still in the lobby
        self.net.advance(12)
        self.assertEqual((self.day["games_started"], self.day["started_event_0"], self.day["start_players_2"]), (1, 1, 1))
        self.assertEqual(self.stats.now_counts()["games_racing"], 1)
        self.assertEqual(self.stats.games_list()[0]["state"], "racing")
        self.poll(host, 1)                                     # back in the lobby after the race
        self.assertEqual(self.stats.now_counts()["games_racing"], 0)
        self.net.advance(12)
        self.assertEqual(self.day["games_started"], 2)         # the next race of the same room
        ok, _ = host.call(21, 2, Writer().u32(gid).bytes())
        self.assertTrue(ok)
        self.assertEqual(self.day["closed_alone"] + self.day["host_left"], 0)     # it had raced
        self.assertEqual(self.stats.public_json()["today"]["games_started"], 2)


class Files(StatsBase):
    def test_pages_and_data_files_with_their_modes(self):
        self.stats.write_pages()
        self.player("Host_Name-ABC", "192.0.2.1")
        self.net.advance(1)
        pub = os.path.join(self.dir, "public")
        self.assertEqual(stat.S_IMODE(os.stat(pub).st_mode), 0o711)
        for f in ("index.html", "stats.json", TOKEN + "/index.html", TOKEN + "/now.json", TOKEN + "/history.jsonl"):
            self.assertEqual(stat.S_IMODE(os.stat(os.path.join(pub, f)).st_mode), 0o644, f)
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.dir, "state", "current.json")).st_mode), 0o600)
        self.assertIn('var DOCS="https://example.org/online"', self.read("public", "index.html"))
        s = json.loads(self.read("public", "stats.json"))
        self.assertEqual((s["now"]["players"], s["today"]["signins"], s["race_detect"]), (1, 1, False))

    def test_no_name_no_address_no_token_in_public_files(self):
        self.stats.write_pages()
        host = self.player("Host_Name-ABC", "192.0.2.1")
        self.room(host, description="Host_Name-ABC")
        self.net.advance(1)
        self.stats.close()
        for rel in (("public", "stats.json"), ("public", TOKEN, "now.json"), ("state", "current.json")):
            text = self.read(*rel)
            self.assertNotIn("Host_Name", text, rel)
            self.assertNotIn("192.0.2.1", text, rel)
        self.assertNotIn(TOKEN, self.read("public", "stats.json") + self.read("public", "index.html"))

    def test_midnight_rollover_and_history(self):
        self.stats.write_pages()
        self.player("Host_Name-ABC")
        today = self.day["date"]
        self.clock.t += 86400
        self.net.advance(1)
        self.assertEqual(self.day["date"], utc_date(self.clock.t))
        self.assertEqual(self.day["signins"], 0)
        lines = self.read("state", "daily.jsonl").splitlines()
        self.assertEqual([json.loads(x)["date"] for x in lines], [today])
        self.assertEqual(self.read("public", TOKEN, "history.jsonl").splitlines(), lines)
        s = json.loads(self.read("public", "stats.json"))
        self.assertEqual([x["date"] for x in s["days"]], [today])

    def test_restart_on_the_same_day_carries_on(self):
        c = self.player("Host_Name-ABC")
        self.net.server.count("relay_datagrams", 5)
        self.stats.close()
        again = self.make_stats()
        net = Net(stats=again)
        self.addCleanup(net.close)
        net.server.count("relay_datagrams", 2)
        again.signin(1000, "192.0.2.1", None)               # the same account signs in after the restart
        again.close()
        self.assertEqual((again.day["signins"], again.day["unique_players"], again.day["relay_packets"]), (2, 1, 7))
        self.assertEqual(again.day["countries"]["HU"], [1, 2])

    def test_start_after_a_day_down_writes_that_day(self):
        self.player("Host_Name-ABC")
        self.stats.close()
        old = self.day["date"]
        self.clock.t += 2 * 86400
        again = self.make_stats()
        self.assertEqual([d["date"] for d in again.history], [old])
        self.assertEqual(again.day["signins"], 0)

    def test_admin_token_rules(self):
        p = os.path.join(self.dir, "token")
        for bad in ("short", "x" * 65, "has space in it here!!"):
            with open(p, "w") as f:
                f.write(bad + "\n")
            self.assertRaises(ValueError, read_token, p)
        with open(p, "w") as f:
            f.write(TOKEN + "\n")
        self.assertEqual(read_token(p), TOKEN)



class Failures(StatsBase):
    """No error in the statistics may reach the game server (sign-in, hosting, the timer)."""

    def break_the_folder(self):
        blocker = os.path.join(self.dir, "not-a-folder")
        with open(blocker, "w") as f:
            f.write("x")
        self.stats.stats_dir = os.path.join(blocker, "state")      # every write in it fails, as on a full disk
        self.net.server.clock = lambda: self.net.now

    def test_a_failing_day_write_stops_neither_sign_in_nor_hosting(self):
        self.player("Before_Midnight")
        self.break_the_folder()
        history = len(self.stats.history)
        self.clock.t += 86400                                       # the next UTC day: the rollover's write fails
        with self.assertLogs("rdvd", "INFO") as logs:
            c = self.player("After_Midnight", "192.0.2.2")
            gid = self.room(c)
            self.assertIn(gid, self.net.server.gatherings)
            self.net.advance(120)                                   # ticks: current.json fails too, once a minute
        self.assertEqual(len(self.stats.history), history + 1)      # the finished day once, not at every tick
        self.assertEqual(self.stats.day["date"], utc_date(self.clock.t))
        failed = [m for m in logs.output if "statistics:" in m]
        self.assertLessEqual(len(failed), 2)                        # one line per hook, no traceback
        self.assertFalse([m for m in logs.output if "Traceback" in m])
        self.assertTrue(self.net.server.counters.get("stats_errors"))

    def test_a_failing_hook_rests_then_is_tried_again(self):
        from rdvd import server as server_mod
        self.net.server.clock = lambda: self.net.now
        calls = []

        def broken(now):
            calls.append(now)
            raise RuntimeError("boom")
        self.stats.tick = broken
        with self.assertLogs("rdvd", "INFO") as logs:
            self.net.advance(30)
            self.assertEqual(len(calls), 1)                         # then resting, not 4 tries a second
            self.net.advance(server_mod.STATS_REST)
            self.assertEqual(len(calls), 2)
            self.net.advance(server_mod.STATS_LOG_EVERY - server_mod.STATS_REST - 60, step=1)
        self.assertLess(len(calls), 2 + server_mod.STATS_LOG_EVERY / server_mod.STATS_REST)   # the rest doubles
        self.assertEqual(len([m for m in logs.output if "statistics:" in m]), 1)          # one line in that hour
        del self.stats.tick                                         # repaired: the class's own tick again
        self.net.advance(server_mod.STATS_REST_MAX + 1)
        self.assertEqual(self.net.server.stats_failing["tick"]["in_row"], 0)

    def test_damaged_state_files_start_the_day_afresh(self):
        state = os.path.join(self.dir, "state")
        with open(os.path.join(state, "current.json"), "w") as f:
            f.write("[1, 2]")
        with open(os.path.join(state, "daily.jsonl"), "w") as f:
            f.write("5\n\"x\"\n{\"date\": \"2026-01-01\", \"signins\": 3}\n")
        st = self.make_stats()
        self.assertEqual([d["date"] for d in st.history], ["2026-01-01"])
        with open(os.path.join(state, "current.json"), "w") as f:
            f.write('{"date": "%s", "seen": ["not a pid"]}' % utc_date(self.clock.t))
        with self.assertLogs("rdvd", "INFO"):
            st = self.make_stats()
        self.assertEqual(st.seen, set())


class SnapshotTest(unittest.TestCase):
    def test_the_ten_minute_log_line_works_with_stats_attached(self):
        net = Net()
        self.addCleanup(net.close)
        net.server.count("relay_datagrams", 3)
        snap = net.server.snapshot()                     # server.stats is the Stats object, not this method
        self.assertEqual(snap["counters"]["relay_datagrams"], 3)


if __name__ == "__main__":
    unittest.main()
