"""Country of an IPv4 address from an offline DB-IP Lite country CSV (CC BY 4.0, https://db-ip.com).

Lines are `first,last,CC` (IPv6 lines are skipped). Nothing is looked up over the network, and the address is
never kept: the caller only stores the two-letter code in its counts. Without a file every address is "??".
"""
import bisect
import logging
import socket
import struct
import sys
from array import array

log = logging.getLogger("rdvd")


def ipv4(text: str):
    """Dotted IPv4 -> int, or None."""
    if text.count(".") != 3:
        return None
    try:
        return struct.unpack("!I", socket.inet_aton(text))[0]
    except OSError:
        return None


class GeoIP:
    """About 5 MB for the full IPv4 table (two arrays of u32 and shared two-letter strings): the unit allows 128 MB."""

    def __init__(self, path=None):
        self.starts, self.ends, self.codes = array("I"), array("I"), []
        if path:
            self.load(path)

    def load(self, path: str):
        starts, ends, codes = array("I"), array("I"), []
        with open(path, encoding="ascii", errors="replace") as f:
            for line in f:
                parts = line.strip().replace('"', "").split(",")
                if len(parts) < 3:
                    continue
                a, b = ipv4(parts[0]), ipv4(parts[1])
                cc = parts[2].strip().upper()
                if a is None or b is None or b < a or len(cc) != 2 or not cc.isalpha() or cc == "ZZ":
                    continue
                starts.append(a)
                ends.append(b)
                codes.append(sys.intern(cc))
        if any(starts[i] > starts[i + 1] for i in range(len(starts) - 1)):     # DB-IP is sorted; sort if not
            order = sorted(range(len(starts)), key=starts.__getitem__)
            starts, ends, codes = array("I", (starts[i] for i in order)), array("I", (ends[i] for i in order)), [codes[i] for i in order]
        self.starts, self.ends, self.codes = starts, ends, codes
        log.info("country database: %d IPv4 ranges", len(starts))

    def __len__(self):
        return len(self.starts)

    def country(self, ip: str) -> str:
        n = ipv4(ip)
        if n is None:
            return "??"
        i = bisect.bisect_right(self.starts, n) - 1
        if i >= 0 and n <= self.ends[i]:
            return self.codes[i]
        return "??"
