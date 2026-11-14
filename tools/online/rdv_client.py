#!/usr/bin/env python3
"""rdv_client.py: a scripted player for a running rdvd, over real UDP (the same calls the game makes).

    python3 rdv_client.py --server localhost --name Probe_One --password secret list
    python3 rdv_client.py --server localhost --name Probe_One --password secret join <gathering id>
    python3 rdv_client.py --server localhost --name Probe_One --password secret host [--hold 60]

The account is created through the guest login if it does not exist. "list" prints the ATVGameSession gatherings,
"join" participates in one and prints the participants' station URLs, "host" registers a room and keeps it open.
A development and monitoring aid: it speaks Rendez-Vous only, it cannot play.
"""
import argparse
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from rdvd import codec                                   # noqa: E402
from rdvd.codec import Reader, Writer                    # noqa: E402
from rdvd.testclient import Client, atv_session          # noqa: E402


class Wire:
    """One UDP socket for both server ports, as the game has; every send is followed by a short read."""

    def __init__(self, host, auth_port, secure_port, wait=0.3):
        self.ports = {"auth": auth_port, "secure": secure_port}
        self.kinds = {auth_port: "auth", secure_port: "secure"}
        self.host, self.wait = socket.gethostbyname(host), wait
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", 0))
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)        # which local address faces the server
        probe.connect((self.host, auth_port))
        self.local = (probe.getsockname()[0], self.sock.getsockname()[1])
        probe.close()
        self.client = None

    def send(self, kind, data):
        self.sock.sendto(data, (self.host, self.ports[kind]))
        self.pump(self.wait)

    def pump(self, seconds):
        end = time.time() + seconds
        while True:
            left = end - time.time()
            if left <= 0:
                return
            self.sock.settimeout(left)
            try:
                data, addr = self.sock.recvfrom(4096)
            except socket.timeout:
                return
            kind = self.kinds.get(addr[1])
            if addr[0] == self.host and kind:
                self.client.receive(kind, data)
                end = min(end, time.time() + 0.05)       # an answer came: only wait for what follows closely
            else:                                        # a peer talking to our station: show it, we cannot answer
                print("datagram from %s:%d: %s" % (addr[0], addr[1], data.hex()), flush=True)


def gatherings(body):
    out = []
    r = Reader(body)
    for _ in range(r.u32()):
        name, g = r.anydata()
        gr = Reader(g)
        out.append({"type": name, "id": gr.u32(), "owner": gr.u32(), "host": gr.u32(), "min": gr.u16(), "max": gr.u16(),
                    "policy": gr.u32(), "policy_arg": gr.u32(), "flags": gr.u32(), "state": gr.u32(),
                    "description": gr.string(), "attributes": list(struct.unpack("<%dI" % (len(g[gr.o:]) // 4), gr.rest()))})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="localhost")
    ap.add_argument("--auth-port", type=int, default=31120)
    ap.add_argument("--secure-port", type=int, default=31121)
    ap.add_argument("--name", required=True)
    ap.add_argument("--password", required=True)
    ap.add_argument("--hold", type=float, default=30.0, help="seconds to stay connected after host/join")
    ap.add_argument("action", choices=("list", "join", "host"))
    ap.add_argument("gid", nargs="?", type=int)
    a = ap.parse_args()
    wire = Wire(a.server, a.auth_port, a.secure_port)
    c = wire.client = Client(wire.send, local_url="prudp:/address=%s;port=%d;sid=15" % wire.local)
    why = c.login(a.name, a.password)
    if why == "retval 0x%08X" % codec.ERR_INVALID_USERNAME:
        print("no such account: creating it ->", "0x%08X" % c.create_account(a.name, a.password))
        why = c.login(a.name, a.password)
    print("login:", why, "pid", c.pid, "public URL", c.public_url)
    if why != "ok":
        return 1
    ok, body = c.call(21, 17, Writer().string("ATVGameSession").u32(0).u32(20).bytes())
    found = gatherings(body)
    for g in found:
        print("gathering", g)
    if not found:
        print("no gatherings")
    if a.action == "join":
        gid = a.gid if a.gid is not None else (found[0]["id"] if found else 0)
        print("Participate(%d):" % gid, c.call(21, 11, Writer().u32(gid).string("").bytes()))
        r = c.call(21, 16, Writer().u32(gid).bytes())
        print("GetParticipantsURLs:", Reader(r[1]).list(lambda x: x.string()) if r and r[0] else r)
    elif a.action == "host":
        r = c.call(21, 1, atv_session(description=a.name, attributes=(1, 0, 0, 0, 0, 0, 1, 0, 0, 0)))
        gid = struct.unpack("<I", r[1])[0]
        print("RegisterGathering ->", gid, "Participate ->", c.call(21, 11, Writer().u32(gid).string("").bytes()))
    if a.action != "list":
        end = time.time() + a.hold
        while time.time() < end:                          # keep the session alive the way the game does
            c.call(20, 10, b"\x02\x01")
            wire.pump(1.0)
            for push in c.links["secure"].pushed:
                print("server call:", push[0], push[1], push[2].hex())
            c.links["secure"].pushed.clear()
    c.links["secure"].disconnect()
    return 0


if __name__ == "__main__":
    sys.exit(main())
