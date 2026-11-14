"""Accounts: name -> pid and the hashed key H (kerberos.py), in SQLite.

What is stored: the name as typed, its lower-case form (uniqueness), H sealed with a secret kept outside the
database, the creation day and the last login day. No address, no e-mail (the client sends a constant dummy one),
no password (the client never sends it).

H is a login-equivalent for this service, so it is not stored as it comes: sealed = H xor HMAC-SHA256(secret,
"acct" | salt)[:16] with a random salt per account. The database file alone opens no account.
"""
import hashlib
import hmac
import os
import re
import sqlite3
import time

FIRST_PID = 1000
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 _.\-]{1,14}[A-Za-z0-9_.\-]$")   # 3-16 characters, printable ASCII subset
RESERVED = {"guest", "admin", "administrator", "root", "system", "server", "quazal", "rendezvous", "moderator",
            "eval", "sandboxprobe", "lobbyadministrator", "backgroundplayer0"}


def valid_name(name: str) -> bool:
    return bool(NAME_RE.match(name)) and "  " not in name and name.lower() not in RESERVED


# The patch's automatic sign-in (SSPC-25) names an account "<Steam name>-<tag>": the tag is 3 characters the DLL
# derives from its per-install key file and the SteamID, so it stays the same when the player renames himself on Steam.
TAG_RE = re.compile(r"-[0-9A-Z]{3}$")


def auto_tag(name: str):
    """The '-XYZ' tag of an automatic sign-in name (upper-case letters and digits), or None."""
    m = TAG_RE.search(name)
    return m.group(0) if m else None


def load_secret(path: str) -> bytes:
    """The server secret (32 random bytes), created on first use with mode 0600."""
    try:
        with open(path, "rb") as f:
            s = f.read()
        if len(s) >= 32:
            return s[:32]
    except FileNotFoundError:
        pass
    s = os.urandom(32)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(s)
    return s


class Accounts:
    def __init__(self, path: str, secret: bytes):
        self.secret = secret
        self.db = sqlite3.connect(path)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute(
            "CREATE TABLE IF NOT EXISTS accounts (pid INTEGER PRIMARY KEY, name TEXT NOT NULL, lname TEXT NOT NULL UNIQUE,"
            " salt BLOB NOT NULL, sealed BLOB NOT NULL, created INTEGER NOT NULL, last_login INTEGER NOT NULL,"
            " disabled INTEGER NOT NULL DEFAULT 0)")
        self.db.commit()

    def _pad(self, salt: bytes) -> bytes:
        return hmac.new(self.secret, b"acct" + salt, hashlib.sha256).digest()[:16]

    def create(self, name: str, h: bytes):
        """-> pid, or None when the name is taken. The caller has checked valid_name and len(h) == 16."""
        salt = os.urandom(16)
        sealed = bytes(a ^ b for a, b in zip(h, self._pad(salt)))
        day = int(time.time()) // 86400
        try:
            row = self.db.execute("SELECT COALESCE(MAX(pid), ?) + 1 FROM accounts", (FIRST_PID - 1,)).fetchone()
            self.db.execute("INSERT INTO accounts (pid, name, lname, salt, sealed, created, last_login) VALUES (?,?,?,?,?,?,?)",
                            (row[0], name, name.lower(), salt, sealed, day, day))
            self.db.commit()
            return row[0]
        except sqlite3.IntegrityError:
            self.db.rollback()
            return None

    def lookup(self, name: str):
        """-> (pid, name as stored, H, disabled) or None."""
        r = self.db.execute("SELECT pid, name, salt, sealed, disabled FROM accounts WHERE lname = ?", (name.lower(),)).fetchone()
        if not r:
            return None
        return r[0], r[1], bytes(a ^ b for a, b in zip(r[3], self._pad(r[2]))), bool(r[4])

    def find_by_key(self, h: bytes, tag: str):
        """-> pid of the account whose name ends with the same tag and whose H is h, or None. Only names with that
        tag are unsealed (a handful); two players who typed the same password are never mixed up unless both names
        also end with the same tag."""
        rows = self.db.execute("SELECT pid, name, salt, sealed FROM accounts WHERE lname LIKE ?", ("%" + tag.lower(),)).fetchall()
        for pid, name, salt, sealed in rows:
            if name.endswith(tag) and hmac.compare_digest(bytes(a ^ b for a, b in zip(sealed, self._pad(salt))), h):
                return pid
        return None

    def rename(self, pid: int, name: str) -> bool:
        """False when another account has the name already."""
        try:
            self.db.execute("UPDATE accounts SET name = ?, lname = ? WHERE pid = ?", (name, name.lower(), pid))
            self.db.commit()
            return True
        except sqlite3.IntegrityError:
            self.db.rollback()
            return False

    def name_of(self, pid: int):
        r = self.db.execute("SELECT name FROM accounts WHERE pid = ?", (pid,)).fetchone()
        return r[0] if r else None

    def created_day(self, pid: int):
        r = self.db.execute("SELECT created FROM accounts WHERE pid = ?", (pid,)).fetchone()
        return r[0] if r else None

    def set_key(self, pid: int, h: bytes):
        salt = os.urandom(16)
        self.db.execute("UPDATE accounts SET salt = ?, sealed = ? WHERE pid = ?",
                        (salt, bytes(a ^ b for a, b in zip(h, self._pad(salt))), pid))
        self.db.commit()

    def touch(self, pid: int):
        self.db.execute("UPDATE accounts SET last_login = ? WHERE pid = ?", (int(time.time()) // 86400, pid))
        self.db.commit()

    def delete(self, name: str) -> bool:
        c = self.db.execute("DELETE FROM accounts WHERE lname = ?", (name.lower(),))
        self.db.commit()
        return c.rowcount > 0

    def count(self) -> int:
        return self.db.execute("SELECT COUNT(*) FROM accounts").fetchone()[0]
