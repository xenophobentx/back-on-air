"""Tickets: the Rendez-Vous "Kerberos" scheme as this client uses it (docs/plan-online.md, section 2.4).

    H = MD5 applied 65000 times to the password          (the client sends H at account creation, never the password)
    K = MD5 applied pid % 1024 more times to H           (the client's key for the tickets of principal pid)
    ticket = RC4(K, session key[16] | u32 secure server pid | Buffer internal ticket) | HMAC-MD5(K, that RC4 output)

The internal ticket is opaque to the client, which hands it back in the CONNECT of the secure connection together
with RC4(session key, u32 pid | u32 connection id | u32 check value) | HMAC-MD5(session key, ...). Ours carries what
the secure side needs (pid, session key, expiry), authenticated with a secret of this server, so no table of issued
tickets is kept.
"""
import hashlib
import hmac
import os
import struct
import time

from .codec import rc4, WireError

SERVER_PID = 2          # "Quazal Rendez-Vous": the principal of the secure server
GUEST_PID = 100
GUEST_NAME = "guest"
GUEST_PASSWORD = "h7fyctiuucf"   # a constant of the retail client; the guest principal can only create accounts
BASE_ROUNDS = 65000
TICKET_LIFETIME = 120   # seconds between RequestTicket and the secure CONNECT


def password_hash(password: str) -> bytes:
    """H: what the client sends (as 32 hex digits) when it creates an account."""
    k = password.encode("utf-8")
    for _ in range(BASE_ROUNDS):
        k = hashlib.md5(k).digest()
    return k


def principal_key(pid: int, h: bytes) -> bytes:
    """K, from H."""
    k = h
    for _ in range(pid % 1024):
        k = hashlib.md5(k).digest()
    return k


def _keystream(secret: bytes, nonce: bytes, n: int) -> bytes:
    return hashlib.sha256(b"rdvd-ticket-key" + secret + nonce).digest()[:n]


def make_internal_ticket(secret: bytes, pid: int, session_key: bytes, now: float = None) -> bytes:
    nonce = os.urandom(8)
    expiry = int((time.time() if now is None else now) + TICKET_LIFETIME)
    sealed = bytes(a ^ b for a, b in zip(session_key, _keystream(secret, nonce, 16)))
    body = nonce + struct.pack("<II", expiry, pid) + sealed
    return body + hmac.new(secret, body, hashlib.sha256).digest()[:16]


def open_internal_ticket(secret: bytes, ticket: bytes, now: float = None):
    """-> (pid, session key). Raises WireError for a forged, damaged or expired ticket."""
    if len(ticket) != 48:
        raise WireError("ticket length")
    body, mac = ticket[:32], ticket[32:]
    if not hmac.compare_digest(mac, hmac.new(secret, body, hashlib.sha256).digest()[:16]):
        raise WireError("ticket not ours")
    expiry, pid = struct.unpack_from("<II", body, 8)
    if (time.time() if now is None else now) > expiry:
        raise WireError("ticket expired")
    return pid, bytes(a ^ b for a, b in zip(body[16:32], _keystream(secret, body[:8], 16)))


def make_ticket(secret: bytes, pid: int, key: bytes, now: float = None) -> bytes:
    """The Buffer content of a Login / RequestTicket answer for principal pid whose key is K."""
    session_key = os.urandom(16)
    internal = make_internal_ticket(secret, pid, session_key, now)
    enc = rc4(key, session_key + struct.pack("<II", SERVER_PID, len(internal)) + internal)
    return enc + hmac.new(key, enc, hashlib.md5).digest()


def open_ticket(key: bytes, ticket: bytes):
    """What the client does with a ticket (for the test client): -> (session key, server pid, internal ticket)."""
    enc, mac = ticket[:-16], ticket[-16:]
    if not hmac.compare_digest(mac, hmac.new(key, enc, hashlib.md5).digest()):
        raise WireError("ticket does not open with this key")
    plain = rc4(key, enc)
    pid, n = struct.unpack_from("<II", plain, 16)
    return plain[:16], pid, plain[24:24 + n]


def make_connect_check(session_key: bytes, pid: int, cid: int, value: int) -> bytes:
    """The second Buffer of the secure CONNECT (client side; used by the test client)."""
    enc = rc4(session_key, struct.pack("<III", pid, cid, value))
    return enc + hmac.new(session_key, enc, hashlib.md5).digest()


def open_connect_check(session_key: bytes, check: bytes):
    """-> (pid, connection id, check value). Raises WireError when the HMAC under the session key is wrong."""
    if len(check) != 28:
        raise WireError("check data length")
    enc, mac = check[:12], check[12:]
    if not hmac.compare_digest(mac, hmac.new(session_key, enc, hashlib.md5).digest()):
        raise WireError("check data HMAC")
    return struct.unpack("<III", rc4(session_key, enc))
