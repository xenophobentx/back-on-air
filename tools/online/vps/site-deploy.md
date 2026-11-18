# Back on Air online pages at `/backonair/`: deploy note for the split-second session (VPS owner)

Nothing here has been deployed. The owner triggers the deploy; the split-second session, which runs
vps4954.rackhostvps.com, does it. It changes only the rdvd service (`splitsecond-rdv`), adds one Caddy snippet,
and adds one link to the shared `/` index. It does not touch `splitsecond-lobby`, `takedawn-lobby`, nftables or ssh.

What it gives (the Takedawn design: rdvd writes static files and Caddy only reads them):

* `https://vps4954.rackhostvps.com/backonair/`: the public page (players and games now, today, games by mode, the
  last 30 days, the open games as neutral rows with no names). It reads `stats.json` every 20 s.
* `https://vps4954.rackhostvps.com/backonair/<token>/`: the owner's page (all-time charts, countries, relay, rooms),
  `now.json` and `history.jsonl`. The token folder is the secret: the parent folder is 0711 (no listing), the page
  says noindex, and rdvd never logs the token.

## The bundle

Build it from the `online-site` branch: `sh tools/online/vps/make-site-bundle.sh /tmp/backonair-site.tar.gz`.
It holds `opt/splitsecond-rdv/` (`rdvd_server.py` and `rdvd/` with `web/` and the fonts), `splitsecond-rdv.service`,
`caddy/backonair.caddy` and this note.

## Steps (as root on the VPS)

```sh
# 0. the bundle
mkdir -p /root/backonair-stage && tar -C /root/backonair-stage -xzf backonair-site.tar.gz
B=/root/backonair-stage/backonair

# 1. the program (root-owned, read-only for the service). Replaces the rdvd that is there.
rsync -a --delete "$B/opt/splitsecond-rdv/" /opt/splitsecond-rdv/
chown -R root:root /opt/splitsecond-rdv && chmod -R go-w /opt/splitsecond-rdv

# 2. the owner's token (40 characters of [A-Za-z0-9_-]); tell the owner the URL, keep it out of logs and chats
python3 -c 'import secrets; print(secrets.token_urlsafe(30))' > /srv/splitsecond-rdv/admin-token
chown ss-rdv:ss-rdv /srv/splitsecond-rdv/admin-token && chmod 600 /srv/splitsecond-rdv/admin-token

# 3. optional: countries for the owner's page (DB-IP Lite, CC BY 4.0, a new file every month).
#    Without the file rdvd still starts; every country is then "??".
curl -fsSL "https://download.db-ip.com/free/dbip-country-lite-$(date -u +%Y-%m).csv.gz" | gunzip > /srv/splitsecond-rdv/dbip-country-lite.csv
chown ss-rdv:ss-rdv /srv/splitsecond-rdv/dbip-country-lite.csv && chmod 600 /srv/splitsecond-rdv/dbip-country-lite.csv

# 4. the unit (adds --stats-dir, --public-dir, --admin-token-file, --geoip and RuntimeDirectory=splitsecond-rdv)
install -m 644 "$B/splitsecond-rdv.service" /etc/systemd/system/splitsecond-rdv.service
systemctl daemon-reload
systemctl restart splitsecond-rdv     # forgets who is online and the open rooms (accounts stay): do it when nobody plays

# 5. Caddy: the snippet, then a restart (the Caddyfile has admin off, so there is no reload)
install -m 644 "$B/caddy/backonair.caddy" /etc/caddy/sites/backonair.caddy
caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile
systemctl restart caddy
```

6. The shared index `/srv/index-public/index.html` (and the split-second repo's `tools/lobby/vps/index/index.html`,
   so the next `deploy.sh` keeps it): add one row inside `<section class="rows">`, beside the other projects:

```html
    <a class="row" href="/backonair/"><span>Split/Second PC</span><b>Back on Air online</b></a>
```

## The unit change (for review)

`ExecStart` names the server by host name now (`--public-address vps4954.rackhostvps.com`, no `--bind`: all
interfaces), it grows at the end, and two lines are new:

```
ExecStart=/usr/bin/python3 -I /opt/splitsecond-rdv/rdvd_server.py --public-address vps4954.rackhostvps.com ... --secret-file /srv/splitsecond-rdv/secret.key --stats-dir /srv/splitsecond-rdv/stats --public-dir /run/splitsecond-rdv --admin-token-file /srv/splitsecond-rdv/admin-token --geoip /srv/splitsecond-rdv/dbip-country-lite.csv
RuntimeDirectory=splitsecond-rdv
RuntimeDirectoryMode=0755
```

The stats folder is inside the existing `ReadWritePaths=/srv/splitsecond-rdv`; systemd makes `/run/splitsecond-rdv`
writable for the service. rdvd sets the modes itself (folder 0711, files 0644, `current.json` 0600), so the unit's
`UMask=0077` does not hide the pages from Caddy. rdvd resolves the host name once at start and stops with a message
if it does not resolve or resolves to a loopback address (an `/etc/hosts` line for the box's own name): check
`getent hosts vps4954.rackhostvps.com` before the restart; it must show the box's public address. A missing token or country file turns that part off, not the server.
Memory: the full DB-IP IPv4 table is about 5 MB in rdvd (limit 128 MB).

## The Caddy snippet (`/etc/caddy/sites/backonair.caddy`)

```
redir /backonair /backonair/ 308

handle_path /backonair/* {
	root * /run/splitsecond-rdv
	@json path *.json *.jsonl
	@notjson not path *.json *.jsonl
	header @json Cache-Control "no-cache"
	header @notjson Cache-Control "public, max-age=3600"
	header Content-Security-Policy "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:; font-src 'self'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'"
	file_server
}
```

It is `takedawn.caddy` with our path and folder, plus a CSP that allows only the pages' inline CSS and script,
the fonts from `/backonair/fonts/` and fetches of its own JSON files.

## Checks after the deploy

```sh
curl -sI https://vps4954.rackhostvps.com/backonair/ | head -1                 # 200
curl -s  https://vps4954.rackhostvps.com/backonair/stats.json | head -c 200   # {"updated":...
curl -s -o /dev/null -w '%{http_code}\n' https://vps4954.rackhostvps.com/backonair/"$(cat /srv/splitsecond-rdv/admin-token)"/now.json   # 200
ls -ld /run/splitsecond-rdv                                                   # drwx--x--x
journalctl -u splitsecond-rdv -n 5                                            # "web pages written (with the owner's page)"
journalctl -u splitsecond-rdv -n 20 | grep 'public address'                   # "public address vps4954... resolves to <the box's address>"
```

## Later switches (no redeploy of the pages)

* Race counts (games racing, races started, players in a race, races by mode): add `--race-detect` to `ExecStart`
  once a live run with three or more games has confirmed the heuristic (stats.py, docstring), then restart.
* Mode names: fill `MODE_NAMES` in `rdvd/stats.py` (event number -> name) after a capture of each mode.
