# tools/online: the server for the PC game's own ONLINE menu

Plan, protocol findings and capture evidence: `docs/plan-online.md`.

| file | what |
|---|---|
| `rdvd/` | the server (Python 3, standard library only) |
| `rdvd_server.py` | launcher (`python3 -I rdvd_server.py --db ... --secret-file ... --public-address ...`) |
| `rdv_client.py` | a scripted player over real UDP: `list`, `join`, `host` (creates its account through the guest login) |
| `rdv_stub.py` | the first capture stub (logs and decodes every datagram; kept for measuring a client) |
| `tests/` | offline unit tests: `python3 -m unittest discover -s tools/online/tests` |
| `vps/` | deployment preparation: systemd unit, firewall line, a note for the project that runs the VPS |

## rdvd

Three UDP ports: **31120** authentication (fixed in the game), **31121** secure (must differ: the client refuses
a secure server on the authentication port), **31122** relay (game traffic between players, used by the DLL's
tunnel once that exists; `--relay-port 0` switches it off).

| module | content |
|---|---|
| `codec.py` | PRUDP v0 packets (Quazal variant), checksum with the access key, RC4 payload wrapping, RMC messages, value readers/writers with length limits, station URLs |
| `transport.py` | connections: handshake, acknowledgements, ordering, resends, fragments, idle ping and drop; no sockets (driven by `datagram()` / `tick()`) |
| `kerberos.py` | key derivation (the client sends MD5 x 65000 of the password, never the password), tickets, the secure CONNECT check; our internal ticket is stateless and authenticated with the server secret |
| `accounts.py` | SQLite: name, sealed key hash, two dates; name rules |
| `limits.py` | token buckets: datagrams, new connections, logins, tickets per name and address, account creation, calls, relay traffic |
| `server.py` | endpoints, call dispatch (`@rmc(kind, protocol, method)`), the secure handshake, sessions |
| `auth.py` | Authentication 10 (Login, LoginEx, RequestTicket), AccountManagement 25 (CreateAccount as guest, ChangePassword) |
| `secure.py` | SecureConnection 11 (Register, RegisterEx, RequestURLs, RequestConnectionData), NATTraversal 3 (probe requests passed on) |
| `matchmaking.py` | MatchMaking 21: gatherings in memory (`ATVGameSession` = Gathering + an opaque tail), the 16 calls the game can make |
| `stats.py`, `geoip.py`, `web/` | per-day statistics (Takedawn's counters as far as our data goes) and the public / owner pages as static files for Caddy at `/backonair/` (`vps/site-deploy.md`); countries from an offline DB-IP Lite CSV; race counts behind `--race-detect` (pending a live test) |
| `misc.py` | Friends 20 method 10 (the Online menu's background poll): an empty list |
| `relay.py` | the relay port: virtual addresses, signed datagrams, forwarding only between two participants of one gathering |
| `testclient.py` | a client that makes the game's calls in the game's order (used by the tests and by `rdv_client.py`) |

Time-outs and limits (`transport.py`, `limits.py`; per source address unless said otherwise):

| what | value |
|---|---|
| ping of a silent secure connection | from 10 s of silence, then every 10 s (home routers forget a UDP mapping after about 30 s) |
| drop of a silent connection | 180 s; 20 minutes while its player is in a room with another player (a race makes no call); authentication port 20 s |
| new port on the same address | followed (the connection's random signature identifies the client) |
| half-open connections (SYN, no CONNECT) | 5 s; 4 per address, 256 per port; when the table is full the oldest half-open one goes, an established one never |
| connections | 8 per address, 512 per port |
| Login | burst 8, then 12 a minute; tickets per account name and address: burst 6, then 6 a minute |
| account creation | burst 5, then 5 an hour; everybody together: burst 500, then 500 an hour; a taken name costs nothing |
| a full rate-limit table (4096 keys) | forgets the key used longest ago |

State of the milestones (plan section 8): deployed on the VPS (`vps4954.rackhostvps.com`) since 2026-11-11. The game signs in,
creates its account, opens the Online menu and hosts a public lobby (M1, M2), and **two real games meet in one public
lobby** through the VPS and see each other (2026-11-11, direct over one LAN). The race does not start yet (both stay in
"WAITING FOR PLAYERS"); not built: the tunnel in the DLL, race start, results, leaderboards (protocols 101/102/26 are
answered "not implemented"). `docs/plan-online.md` section 12 has the exact state and the next steps.

Run locally: `python3 tools/online/rdvd_server.py --db /tmp/a.sqlite --secret-file /tmp/s.key --log-calls`, the
game with `online_server = localhost` in `splitsecond.conf`. `--log-calls` prints names and call bytes: for
development only.
