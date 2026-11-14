"""Sample web files for a look at the two pages without a live server.

    python3 tests/site_sample.py OUT_DIR [--race-detect]

Writes OUT_DIR/state (daily.jsonl with 45 made-up days, current.json) and OUT_DIR/public (index.html,
stats.json, sample-owner-token-0000/...). Today's numbers come from real calls into an in-process server (players
signing in, public and private rooms, a race when --race-detect is given). Serve OUT_DIR/public with any static
web server, e.g. `python3 -m http.server -d OUT_DIR/public 8765`, and open / and /sample-owner-token-0000/.
"""
import json
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from support import Net                      # noqa: E402
from rdvd.codec import Writer                # noqa: E402
from rdvd.stats import Stats, blank_day, compact, utc_date   # noqa: E402
from rdvd.testclient import atv_session      # noqa: E402

TOKEN = "sample-owner-token-0000"


def made_up_history(n, race_detect):
    rnd, out, today = random.Random(7), [], int(time.time()) // 86400
    for i in range(n, 0, -1):
        d = blank_day(utc_date((today - i) * 86400))
        u = max(0, int(6 + 10 * (n - i) / n + rnd.randint(-3, 5)))
        d.update(unique_players=u, signins=u + rnd.randint(0, u), returning_players=max(0, u - rnd.randint(1, 4)),
                 peak_players=max(1, u // 2), peak_sessions=max(1, u // 2 + 1), games_created=u // 2 + rnd.randint(0, 3),
                 race_detect=race_detect and i < 20)
        d["hour_peak"] = [max(0, int(d["peak_players"] * (0.3 + 0.7 * (18 <= h <= 23)) - rnd.randint(0, 2))) for h in range(24)]
        d["games_private"] = rnd.randint(0, 2)
        d["games_public"] = d["games_created"] - d["games_private"]
        d["created_event_0"] = d["games_created"]
        if d["race_detect"]:
            d["games_started"] = d["started_event_0"] = max(0, d["games_created"] - rnd.randint(0, 3))
            d["start_players_3"] = d["games_started"] // 2
            d["start_players_4"] = d["games_started"] - d["start_players_3"]
        d["closed_alone"] = rnd.randint(0, 3)
        d["closed_alone_never"] = d["closed_alone"] // 2
        d["closed_alone_secs"] = d["closed_alone"] * rnd.randint(30, 400)
        d["closed_alone_lt5m"] = d["closed_alone"]
        d["host_left"] = rnd.randint(0, 2)
        d["host_left_players"] = d["host_left"] * 2
        d["host_left_close"] = d["host_left"]
        cc = {"HU": [u // 2, u], "DE": [u // 4, u // 3], "GB": [u // 6, u // 5], "US": [u // 8, u // 6], "??": [1, 1]}
        d["countries"] = {k: v for k, v in cc.items() if v[0]}
        out.append(d)
    return out


def main():
    out = os.path.abspath(sys.argv[1])
    race_detect = "--race-detect" in sys.argv
    state, public = os.path.join(out, "state"), os.path.join(out, "public")
    os.makedirs(state, exist_ok=True)
    with open(os.path.join(state, "daily.jsonl"), "w") as f:
        for d in made_up_history(45, race_detect):
            f.write(compact(d) + "\n")
    for name in ("current.json",):
        try:
            os.remove(os.path.join(state, name))
        except FileNotFoundError:
            pass
    stats = Stats(state, public, TOKEN, None, race_detect, "https://example.org/back-on-air#online")
    net = Net(stats=stats)
    stats.write_pages()
    players = []
    for i, name in enumerate(["Alpha-AAA", "Bravo-BBB", "Charlie-CCC", "Delta-DDD", "Echo-EEE", "Fox-FFF", "Golf-GGG"]):
        c = net.client("player%d.test" % (i + 1))
        c.create_account(name, "pw")
        c.login(name, "pw")
        players.append(c)

    def room(host, attrs, joiners):
        ok, body = host.call(21, 1, atv_session(description="(host name)", attributes=attrs))
        gid = int.from_bytes(body[:4], "little")
        for c in [host] + joiners:
            c.call(21, 11, Writer().u32(gid).string("").bytes())
        return gid

    room(players[0], (1, 0, 0, 0, 0, 0, 1, 0, 0, 0), [players[1], players[2]])
    room(players[3], (1, 0, 0, 0, 0, 0, 1, 0, 0, 0), [])
    room(players[4], (2, 0, 0, 0, 0, 0, 1, 0, 0, 0), [players[5]])
    for _ in range(4):                     # the lobbies poll; the first room's host goes quiet (a race) after that
        for c in players[3:]:
            c.call(20, 10, b"\x00")
        net.advance(3)
    gid = room(players[6], (1, 0, 0, 0, 0, 0, 1, 0, 0, 0), [])
    players[6].call(21, 2, Writer().u32(gid).bytes())
    stats.last_public = 0
    stats.tick(net.now)
    stats.close()
    print(json.dumps(stats.public_json()["now"]))
    print("pages:", public, "owner folder:", TOKEN)


if __name__ == "__main__":
    main()
