# rdvd update of 2026-11-14 (review fixes): deploy note for the split-second session (VPS owner)

Nothing here has been deployed. The owner triggers the deploy; the split-second session, which runs
vps4954.rackhostvps.com, does it. It replaces the program in `/opt/splitsecond-rdv/` and restarts
`splitsecond-rdv`. No change to the unit, nftables, Caddy, ssh, `splitsecond-lobby` or `takedawn-lobby`. The
accounts database and `secret.key` stay as they are (no schema change).

## What changes for players

* A game that sends nothing during a race is no longer dropped after 3 minutes: the server pings a silent
  connection from 10 s on, every 10 s, and keeps a player who is in a room with another player for 20 minutes of
  silence. A player alone, or not in a room, still goes after 180 s.
* A client whose router gives it a new port during a silence is followed (same public address, same random
  connection signature).
* Account creation: 5 at once, then 5 an hour per address; everybody together 500 at once, then 500 an hour (was
  3 per address and 30 at once, 60 an hour in all). A name that is taken costs no token.
* Login tickets per account name are counted per name and source address: somebody else's Login calls for a name
  no longer lock its owner out.
* Half-open connections (SYN, no CONNECT) expire after 5 s (was 10 s), at most 4 per address and 256 per port; when
  the table is full the oldest half-open one is evicted, an established one never.

New counters in the 10-minute `stats` log line: `port_changed`, `half_open_evicted`.

## Which branch

The program comes from the branch that is deployed now. Check it first:
`journalctl -u splitsecond-rdv | grep -c "not callable"` must be 0; if `/opt/splitsecond-rdv/rdvd/stats.py`
exists, the box runs an `online-site` build: deploy from `online-site` (it has these fixes too, and the statistics
are isolated from the game server there). Otherwise deploy from `release`.

## Steps (as root on the VPS)

```sh
# 0. a copy of the running program, for a roll-back
cp -a /opt/splitsecond-rdv /root/splitsecond-rdv.before-2026-11-14

# 1. the program (from the repo checkout of the chosen branch: tools/online/rdvd_server.py and tools/online/rdvd/;
#    online-site: sh tools/online/vps/make-site-bundle.sh gives the same tree in opt/splitsecond-rdv/)
rsync -a --delete --exclude __pycache__ <checkout>/tools/online/rdvd/ /opt/splitsecond-rdv/rdvd/
install -m 0644 <checkout>/tools/online/rdvd_server.py /opt/splitsecond-rdv/rdvd_server.py
chown -R root:root /opt/splitsecond-rdv && chmod -R go-w /opt/splitsecond-rdv

# 2. restart when nobody plays: a restart ends every open room and session (accounts stay)
systemctl restart splitsecond-rdv
journalctl -u splitsecond-rdv -n 20 --no-pager      # "listening on ..." with the account count
```

Roll-back: `rsync -a --delete /root/splitsecond-rdv.before-2026-11-14/ /opt/splitsecond-rdv/` and restart.

## Check after the deploy

* `journalctl -u splitsecond-rdv -n 50`: no traceback.
* A sign-in from a game (or `python3 tools/online/rdv_client.py` against the box) still works.
* Unit tests before copying: `python3 -m unittest discover -s tools/online/tests` (all pass on 2026-11-14).

## A later move to another host name

The DLL's built-in server is `kOnlineDefaultServer` in `src/online_conf.h`; the shipped `splitsecond.conf` has
`online_server =` (empty), so it follows the DLL. A move needs: the new name pointing at the box, the unit's
`--public-address` set to it (the login answer gives clients that name for the secure port), and a DLL release with
the new default. The old name should keep pointing at the box until the old DLLs are gone.
