from __future__ import annotations

from ..db import Session, iso

_COLUMNS = "id, username, password_hash, role, created_at, last_login_at"


def public(row: dict | None) -> dict | None:
    """User row without the password hash - the only shape that ever leaves the backend."""
    if row is None:
        return None
    return {"id": row["id"], "username": row["username"], "role": row["role"],
            "created_at": iso(row["created_at"]), "last_login_at": iso(row.get("last_login_at"))}


def count(tx: Session) -> int:
    return int(tx.scalar("SELECT COUNT(*) FROM users") or 0)


def create(tx: Session, username: str, password_hash: str, role: str) -> int:
    return tx.insert("INSERT INTO users (username, password_hash, role) VALUES (%s, %s, %s)",
                     (username, password_hash, role))


def by_username(tx: Session, username: str) -> dict | None:
    return tx.fetch_one(f"SELECT {_COLUMNS} FROM users WHERE username = %s", (username,))


def by_id(tx: Session, user_id: int) -> dict | None:
    return tx.fetch_one(f"SELECT {_COLUMNS} FROM users WHERE id = %s", (user_id,))


def touch_login(tx: Session, user_id: int, when: str) -> None:
    tx.execute("UPDATE users SET last_login_at = %s WHERE id = %s", (when, user_id))
