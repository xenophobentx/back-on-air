"""Rate limits: token buckets per key (a source address, a name), kept in a bounded table."""
from collections import OrderedDict


class Buckets:
    """allow(key, now) spends one token; a bucket holds `burst` tokens and gains `rate` per second.

    A full table forgets the key used longest ago: a flood of forged source addresses then costs the limits their
    memory of those addresses (a fresh bucket is a full one), never a real player his answer, and no scan per packet."""

    def __init__(self, rate: float, burst: float, max_keys: int = 4096):
        self.rate, self.burst, self.max_keys = rate, burst, max_keys
        self.t = OrderedDict()

    def allow(self, key, now: float, cost: float = 1.0) -> bool:
        b = self.t.get(key)
        if b is None:
            if len(self.t) >= self.max_keys:
                self.t.popitem(last=False)
            tokens = self.burst
        else:
            tokens = min(self.burst, b[0] + (now - b[1]) * self.rate)
            self.t.move_to_end(key)
        if tokens < cost:
            self.t[key] = (tokens, now)
            return False
        self.t[key] = (tokens - cost, now)
        return True

    def prune(self, now: float):
        full = [k for k, (tok, t) in self.t.items() if tok + (now - t) * self.rate >= self.burst]
        for k in full:
            del self.t[k]


class Limits:
    """The limits of the server, in one place. Values are per source address unless said otherwise."""

    def __init__(self):
        self.packets = Buckets(150.0, 300.0)          # datagrams per second
        self.syn = Buckets(0.5, 12.0)                 # new connections: 30 a minute, burst 12
        self.login = Buckets(0.2, 8.0)                # Login calls: 12 a minute, burst 8
        self.login_name = Buckets(0.1, 6.0)           # tickets per account name and source address: 6 a minute
        self.create = Buckets(5.0 / 3600.0, 5.0)      # accounts created: burst 5, then 5 an hour
        # Accounts created by everybody: a ceiling against a flood from many addresses, not a launch-night queue
        # (100 addresses at the per-address limit reach it). Burst 500, then 500 an hour.
        self.create_global = Buckets(500.0 / 3600.0, 500.0)
        self.calls = Buckets(20.0, 60.0)              # RMC calls per connection per second
        self.max_connections = 512                    # per port
        self.max_per_address = 8
        self.max_half_open = 256                      # per port: SYN acknowledged, no CONNECT yet (oldest evicted)
        self.max_half_open_per_address = 4
        self.max_gatherings = 256

    def prune(self, now: float):
        for b in (self.packets, self.syn, self.login, self.login_name, self.create, self.create_global, self.calls):
            b.prune(now)
