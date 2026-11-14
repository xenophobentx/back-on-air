"""Test support: a server and any number of clients wired together in one process, with a clock of their own."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from rdvd.server import Server          # noqa: E402
from rdvd.testclient import Client      # noqa: E402


class Net:
    def __init__(self, **server_args):
        self.dir = tempfile.TemporaryDirectory()
        self.now = 1000.0
        self.clients = {}       # address -> Client
        self.sent = {"auth": [], "secure": []}     # every datagram the server sent: (bytes, address)
        self.drop_to_client = False
        self.relayed = []       # every datagram the relay port sent: (bytes, address)
        self.server = Server(os.path.join(self.dir.name, "accounts.sqlite"), b"S" * 32, "rdv.test",
                             send_auth=lambda b, a: self._to_client("auth", b, a),
                             send_secure=lambda b, a: self._to_client("secure", b, a),
                             send_relay=lambda b, a: self.relayed.append((b, a)), **server_args)

    def _to_client(self, kind, data, addr):
        self.sent[kind].append((data, addr))
        if not self.drop_to_client and addr in self.clients:
            self.clients[addr].receive(kind, data)

    def client(self, ip="player1.test", port=9103) -> Client:
        addr = (ip, port)
        c = Client(lambda kind, data: self.server.datagram(kind, data, addr, self.now),
                   local_url="prudp:/address=lan%d.test;port=9103;sid=15" % (len(self.clients) + 10))
        self.clients[addr] = c
        return c

    def raw(self, kind, data, addr=("player9.test", 9103)):
        """A datagram straight into the server; -> what the server sent back to that address."""
        n = len(self.sent[kind])
        self.server.datagram(kind, data, addr, self.now)
        return [d for d, a in self.sent[kind][n:] if a == addr]

    def advance(self, seconds, step=0.25):
        end = self.now + seconds
        while self.now < end:
            self.now += step
            self.server.tick(self.now)

    def close(self):
        self.server.accounts.db.close()
        self.dir.cleanup()
