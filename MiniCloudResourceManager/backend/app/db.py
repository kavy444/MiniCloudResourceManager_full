"""Database access: a small thread-safe connection pool, transactions and parameterized queries.

All SQL in the project is written with %s placeholders and values are ALWAYS passed as parameters (never
formatted into the SQL text), so user input cannot change the meaning of a statement.

The MySQL driver is PyMySQL. The `Backend` seam exists so the unit tests can run the very same repository code
against SQLite (see backend/tests/sqlite_backend.py) - production uses MySQLBackend only.
"""
from __future__ import annotations

import queue
import threading
from contextlib import contextmanager
from datetime import datetime, timezone
from typing import Any, Iterable, Iterator, Sequence

from .errors import unavailable


class PoolTimeout(RuntimeError):
    pass


def utcnow_str() -> str:
    """UTC wall-clock time in the format MySQL DATETIME accepts."""
    return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


def iso(value: Any) -> str | None:
    """DATETIME (datetime object from MySQL, text from SQLite) -> ISO-8601 string in UTC."""
    if value is None:
        return None
    if isinstance(value, datetime):
        return value.replace(tzinfo=None).isoformat(timespec="seconds") + "Z"
    return str(value).replace(" ", "T")[:19] + "Z"


class Backend:
    """What the pool needs to know about a database engine."""

    dialect = "generic"
    lock_suffix = ""                 # appended to SELECTs that must lock rows
    integrity_errors: tuple = ()     # driver exception classes meaning "constraint violated"
    connection_errors: tuple = ()    # driver exception classes meaning "this connection is dead"

    def connect(self):                # pragma: no cover - implemented by subclasses
        raise NotImplementedError

    def adapt(self, sql: str) -> str:
        return sql

    def begin(self, conn) -> None:
        """Start a transaction explicitly if the driver needs it."""

    def ping(self, conn) -> None:
        """Raise if the connection is no longer usable."""


class MySQLBackend(Backend):
    dialect = "mysql"
    lock_suffix = " FOR UPDATE"

    def __init__(self, settings):
        import pymysql  # imported lazily so the rest of the package can be imported without the driver
        import pymysql.cursors

        self._pymysql = pymysql
        self._settings = settings
        self.integrity_errors = (pymysql.err.IntegrityError,)
        self.connection_errors = (pymysql.err.OperationalError, pymysql.err.InterfaceError)

    def connect(self):
        s = self._settings
        return self._pymysql.connect(
            host=s.db_host,
            port=s.db_port,
            user=s.db_user,
            password=s.db_password,
            database=s.db_name,
            charset="utf8mb4",
            cursorclass=self._pymysql.cursors.DictCursor,
            autocommit=False,
            connect_timeout=5,
            read_timeout=30,
            write_timeout=30,
            init_command="SET time_zone = '+00:00'",   # DATETIME columns are stored in UTC
        )

    def ping(self, conn) -> None:
        conn.ping(reconnect=True)


class Session:
    """What repositories use: thin helpers around one connection inside one transaction."""

    def __init__(self, conn, backend: Backend):
        self._conn = conn
        self._backend = backend

    @property
    def lock_suffix(self) -> str:
        return self._backend.lock_suffix

    def _run(self, sql: str, params: Sequence | None):
        cur = self._conn.cursor()
        cur.execute(self._backend.adapt(sql), tuple(params or ()))
        return cur

    def execute(self, sql: str, params: Sequence | None = None) -> int:
        cur = self._run(sql, params)
        try:
            return cur.rowcount
        finally:
            cur.close()

    def insert(self, sql: str, params: Sequence | None = None) -> int:
        cur = self._run(sql, params)
        try:
            return int(cur.lastrowid)
        finally:
            cur.close()

    def execute_many(self, sql: str, rows: Iterable[Sequence]) -> int:
        rows = [tuple(r) for r in rows]
        if not rows:
            return 0
        cur = self._conn.cursor()
        try:
            cur.executemany(self._backend.adapt(sql), rows)
            return cur.rowcount
        finally:
            cur.close()

    def fetch_one(self, sql: str, params: Sequence | None = None) -> dict | None:
        cur = self._run(sql, params)
        try:
            row = cur.fetchone()
            return dict(row) if row is not None else None
        finally:
            cur.close()

    def fetch_all(self, sql: str, params: Sequence | None = None) -> list[dict]:
        cur = self._run(sql, params)
        try:
            return [dict(r) for r in cur.fetchall()]
        finally:
            cur.close()

    def scalar(self, sql: str, params: Sequence | None = None) -> Any:
        row = self.fetch_one(sql, params)
        return None if row is None else next(iter(row.values()))


class Database:
    """Bounded pool of connections. `with db.transaction() as tx:` commits on success, rolls back on any error."""

    def __init__(self, backend: Backend, pool_size: int = 5, acquire_timeout_s: float = 10.0):
        self.backend = backend
        self._size = pool_size
        self._timeout = acquire_timeout_s
        self._idle: "queue.LifoQueue" = queue.LifoQueue()
        self._lock = threading.Lock()   # protects _created
        self._created = 0
        self._closed = False

    @property
    def integrity_errors(self) -> tuple:
        return self.backend.integrity_errors

    # ---- pool -----------------------------------------------------------------------------------
    def _acquire(self):
        if self._closed:
            raise PoolTimeout("database pool is closed")
        while True:
            try:
                conn = self._idle.get_nowait()
            except queue.Empty:
                conn = None
            if conn is not None:
                try:
                    self.backend.ping(conn)
                    return conn
                except Exception:
                    self._discard(conn)
                    continue
            with self._lock:
                if self._created < self._size:
                    self._created += 1
                    create = True
                else:
                    create = False
            if create:
                try:
                    return self.backend.connect()
                except BaseException:
                    with self._lock:
                        self._created -= 1
                    raise
            try:
                conn = self._idle.get(timeout=self._timeout)
            except queue.Empty as exc:
                raise PoolTimeout("no free database connection (pool exhausted)") from exc
            try:
                self.backend.ping(conn)
                return conn
            except Exception:
                self._discard(conn)

    def _release(self, conn) -> None:
        if self._closed:
            self._discard(conn)
        else:
            self._idle.put(conn)

    def _discard(self, conn) -> None:
        try:
            conn.close()
        except Exception:
            pass
        with self._lock:
            self._created -= 1

    # ---- public API -----------------------------------------------------------------------------
    @contextmanager
    def transaction(self) -> Iterator[Session]:
        try:
            conn = self._acquire()
        except PoolTimeout as exc:
            raise unavailable("The database is busy, please retry in a moment") from exc
        except Exception as exc:
            if self.backend.connection_errors and isinstance(exc, self.backend.connection_errors):
                raise unavailable("The database is not reachable. Is MySQL running and is .env correct?") from exc
            raise
        broken = False
        try:
            self.backend.begin(conn)
            yield Session(conn, self.backend)
            conn.commit()
        except BaseException as exc:
            try:
                conn.rollback()
            except Exception:
                broken = True
            if self.backend.connection_errors and isinstance(exc, self.backend.connection_errors):
                broken = True
            raise
        finally:
            if broken:
                self._discard(conn)
            else:
                self._release(conn)

    def ping(self) -> bool:
        try:
            with self.transaction() as tx:
                tx.fetch_one("SELECT 1 AS ok")
            return True
        except Exception:
            return False

    def close(self) -> None:
        self._closed = True
        while True:
            try:
                conn = self._idle.get_nowait()
            except queue.Empty:
                break
            self._discard(conn)
