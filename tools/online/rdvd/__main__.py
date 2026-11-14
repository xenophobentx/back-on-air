"""python3 -m rdvd: the Split/Second PC Rendez-Vous server on two UDP ports (authentication, secure).

    python3 -I -m rdvd --db /srv/splitsecond-rdv/accounts.sqlite --secret-file /srv/splitsecond-rdv/secret.key \
                       --public-address <the host name clients reach this host under> [--bind ADDRESS]
                       [--auth-port 31120] [--secure-port 31121] [--relay-port 31122] [--log-calls] [--verbose]
                       [--stats-dir DIR] [--public-dir DIR] [--admin-token-file FILE] [--geoip FILE] [--race-detect]
    python3 -I -m rdvd --db ... --secret-file ... --delete-account NAME      (maintenance, server stopped or not)

The authentication port is fixed in the game (31120). The secure port must be another one (the client refuses a
secure server on the authentication port) and is told to the client in the login answer, with --public-address.
--public-address takes a host name (or an address); it is resolved to IPv4 once at start, because the login answer
carries a numeric address. A name that does not resolve, or resolves to a loopback address when it is not
"localhost" (an /etc/hosts line for the machine's own name), stops the server with a message. --bind: no value =
all interfaces.
"""
import argparse
import asyncio
import logging
import signal
import socket
import sys
import time

from .accounts import Accounts, load_secret
from .geoip import GeoIP
from .server import Server
from .stats import Stats, read_token

log = logging.getLogger("rdvd")


class Port(asyncio.DatagramProtocol):
    def __init__(self, server, kind):
        self.server, self.kind, self.transport = server, kind, None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        try:
            self.server.datagram(self.kind, data, addr[:2], time.monotonic())
        except Exception:                      # one packet must never stop the service
            log.exception("error handling a datagram on the %s port", self.kind)

    def send(self, data, addr):
        if self.transport is not None:
            self.transport.sendto(data, addr)


async def run(args):
    secret = load_secret(args.secret_file)
    token = geoip = None
    if args.admin_token_file:                  # a missing or bad file turns the owner's page off, not the server
        try:
            token = read_token(args.admin_token_file)          # never logged
        except (OSError, ValueError) as e:
            log.info("owner's page off: %s", getattr(e, "strerror", None) or e)
    if args.geoip:
        try:
            geoip = GeoIP(args.geoip)
        except OSError as e:
            log.info("no country database (%s): countries are counted as ??", e.strerror or e)
    stats = Stats(args.stats_dir, args.public_dir, token, geoip, args.race_detect, args.docs_url)
    server = Server(args.db, secret, args.public_address, args.auth_port, args.secure_port, args.access_key,
                    log_calls=args.log_calls, notifications=args.notifications, stats=stats)
    server.stat("write_pages")                 # like every statistics call: an error is logged, the server starts
    loop = asyncio.get_running_loop()
    stop = asyncio.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        try:
            loop.add_signal_handler(sig, stop.set)
        except (NotImplementedError, RuntimeError):
            pass
    ports = [("auth", args.auth_port, server.auth), ("secure", args.secure_port, server.secure)]
    if args.relay_port:
        ports.append(("relay", args.relay_port, server.relay))
    for kind, port, endpoint in ports:
        proto = Port(server, kind)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((args.bind, port))                         # "" = all interfaces
        await loop.create_datagram_endpoint(lambda p=proto: p, sock=sock)
        endpoint.send = proto.send
    log.info("listening on %s, UDP %d (authentication), %d (secure, announced as %s), relay %s; %d accounts",
             args.bind or "all interfaces", args.auth_port, args.secure_port, args.public_address, args.relay_port or "off", server.accounts.count())
    last_stats = time.monotonic()
    while not stop.is_set():
        try:
            await asyncio.wait_for(stop.wait(), 0.25)
        except asyncio.TimeoutError:
            pass
        now = time.monotonic()
        try:
            server.tick(now)
        except Exception:
            log.exception("error in the timer")
        if now - last_stats > 600:
            last_stats = now
            log.info("stats %s", server.snapshot())
    server.stat("close")                       # today's counts survive the restart (current.json)
    log.info("stopped")


def resolve_public(name: str) -> str:
    """--public-address -> the dotted IPv4 address the login answer names. Raises ValueError with the reason."""
    try:
        addr = socket.gethostbyname(name)
    except OSError as e:
        raise ValueError("--public-address %s does not resolve (%s)" % (name, e)) from None
    if addr.startswith("127.") and name.lower() not in ("localhost", "localhost.") and not name.startswith("127."):
        raise ValueError("--public-address %s resolves to the loopback address %s (an /etc/hosts line?): clients "
                         "could not reach that; fix the name or give the public address" % (name, addr))
    return addr


def main():
    ap = argparse.ArgumentParser(prog="rdvd", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", required=True)
    ap.add_argument("--secret-file", required=True)
    ap.add_argument("--public-address", default="localhost", help="host name the clients reach this host under")
    ap.add_argument("--bind", default="", help="local address to listen on (default: all interfaces)")
    ap.add_argument("--auth-port", type=int, default=31120)
    ap.add_argument("--secure-port", type=int, default=31121)
    ap.add_argument("--relay-port", type=int, default=31122, help="0 = no relay port")
    ap.add_argument("--access-key", default="3wyEgW4x")
    ap.add_argument("--notifications", action="store_true", help="send participation notifications (layout unverified)")
    ap.add_argument("--log-calls", action="store_true", help="log every call with its bytes (development: shows names)")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--delete-account", metavar="NAME")
    ap.add_argument("--stats-dir", help="per-day statistics: daily.jsonl, current.json (nothing is written without it)")
    ap.add_argument("--public-dir", help="the public page's folder for the web server (default <stats-dir>/public)")
    ap.add_argument("--admin-token-file", help="one line of 20-64 [A-Za-z0-9_-]: the owner's page folder name")
    ap.add_argument("--geoip", help="DB-IP Lite country CSV (offline; only the country code is counted)")
    ap.add_argument("--race-detect", action="store_true",
                    help="count races with the host-silence heuristic (pending a live test, stats.py)")
    ap.add_argument("--docs-url", default="", help="the public page's 'how to play online' link")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format="%(asctime)s %(message)s", stream=sys.stdout)
    if args.delete_account:
        ok = Accounts(args.db, load_secret(args.secret_file)).delete(args.delete_account)
        print("deleted" if ok else "no such account")
        return 0 if ok else 1
    if args.auth_port == args.secure_port:
        ap.error("the secure port must differ from the authentication port")
    try:
        public = resolve_public(args.public_address)
    except ValueError as e:
        ap.error(str(e))
    if public != args.public_address:
        log.info("public address %s resolves to %s", args.public_address, public)
    args.public_address = public
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
