"""Passwords, tokens and login throttling.

* Passwords: scrypt (memory-hard, in the Python standard library) with a random 16-byte salt per user.
* Tokens: signed JWT (HS256) that only carries the user id and an expiry. The role is NOT trusted from the
  token: every request loads the user from the database, so a deleted user's token stops working.
* Login limiter: counts failed logins per (username, client address) inside a window. It is shared by all
  request threads, so it is protected by a real lock.

This is reasonable protection for a college project; it is not a claim that the system is hardened for the
public internet (no HTTPS termination, no account recovery, in-memory limiter resets on restart).
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import os
import threading
import time
from collections import deque
from datetime import datetime, timedelta, timezone

from .errors import too_many, unauthorized

_N, _R, _P, _DKLEN = 2 ** 14, 8, 1, 32
_MAXMEM = 64 * 1024 * 1024


def hash_password(password: str) -> str:
    salt = os.urandom(16)
    digest = hashlib.scrypt(password.encode("utf-8"), salt=salt, n=_N, r=_R, p=_P, dklen=_DKLEN, maxmem=_MAXMEM)
    return "scrypt${}${}${}${}${}".format(_N, _R, _P, base64.b64encode(salt).decode(), base64.b64encode(digest).decode())


def verify_password(password: str, stored: str) -> bool:
    try:
        scheme, n, r, p, salt_b64, digest_b64 = stored.split("$")
        if scheme != "scrypt":
            return False
        salt = base64.b64decode(salt_b64)
        expected = base64.b64decode(digest_b64)
        actual = hashlib.scrypt(password.encode("utf-8"), salt=salt, n=int(n), r=int(r), p=int(p),
                                dklen=len(expected), maxmem=_MAXMEM)
    except (ValueError, TypeError):
        return False
    return hmac.compare_digest(actual, expected)


# A valid hash of a random password, used so that "unknown user" takes as long as "wrong password".
DUMMY_HASH = hash_password(base64.b64encode(os.urandom(12)).decode())

ISSUER = "mini-cloud-resource-manager"


def create_token(user_id: int, secret: str, ttl_minutes: int) -> tuple[str, int]:
    import jwt  # PyJWT

    now = datetime.now(timezone.utc)
    payload = {"sub": str(user_id), "iss": ISSUER, "iat": now, "exp": now + timedelta(minutes=ttl_minutes)}
    return jwt.encode(payload, secret, algorithm="HS256"), ttl_minutes * 60


def decode_token(token: str, secret: str) -> int:
    """Returns the user id or raises ApiError 401."""
    import jwt

    try:
        claims = jwt.decode(token, secret, algorithms=["HS256"], issuer=ISSUER, options={"require": ["exp", "sub", "iss"]})
        return int(claims["sub"])
    except jwt.ExpiredSignatureError as exc:
        raise unauthorized("Your session has expired, please log in again") from exc
    except (jwt.InvalidTokenError, ValueError, KeyError) as exc:
        raise unauthorized("Invalid authentication token") from exc


class LoginLimiter:
    """Allows `max_failures` failed logins per key inside `window_s` seconds."""

    def __init__(self, max_failures: int = 5, window_s: int = 300, clock=time.monotonic):
        self._max = max_failures
        self._window = window_s
        self._clock = clock
        self._lock = threading.Lock()
        self._failures: dict[str, deque] = {}

    def _prune(self, key: str, now: float) -> deque:
        q = self._failures.get(key)
        if q is None:
            return deque()
        while q and now - q[0] > self._window:
            q.popleft()
        if not q:
            self._failures.pop(key, None)
        return q

    def check(self, key: str) -> None:
        with self._lock:
            now = self._clock()
            q = self._prune(key, now)
            if len(q) >= self._max:
                wait = int(self._window - (now - q[0])) + 1
                raise too_many(f"Too many failed logins. Try again in {wait} seconds.")

    def record_failure(self, key: str) -> None:
        with self._lock:
            now = self._clock()
            q = self._failures.setdefault(key, deque())
            while q and now - q[0] > self._window:
                q.popleft()
            q.append(now)
            if len(self._failures) > 5000:        # keep memory bounded
                for k in list(self._failures):
                    self._prune(k, now)

    def reset(self, key: str) -> None:
        with self._lock:
            self._failures.pop(key, None)
