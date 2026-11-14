"""TEST-ONLY database backend. It lets the repository/service code run against SQLite in the unit tests because
MySQL is not available in every environment. Production uses app.db.MySQLBackend.

What this does NOT prove: MySQL-specific behaviour (row locking with SELECT ... FOR UPDATE, InnoDB isolation,
MySQL's CHECK enforcement, charset handling). Those need a real MySQL run (see README, 'Tests')."""
from __future__ import annotations

import os
import re
import sqlite3
import tempfile

from app.db import Backend, Database


def translate_ddl(sql: str) -> str:
    sql = re.sub(r"BIGINT\s+NOT NULL\s+AUTO_INCREMENT\s+PRIMARY KEY", "INTEGER PRIMARY KEY AUTOINCREMENT", sql)
    sql = re.sub(r"\)\s*ENGINE=InnoDB[^;]*$", ")", sql.strip())
    return sql


class SQLiteBackend(Backend):
    dialect = "sqlite"
    lock_suffix = ""                       # SQLite locks the whole database in BEGIN IMMEDIATE
    integrity_errors = (sqlite3.IntegrityError,)
    connection_errors = ()

    def __init__(self, path: str):
        self.path = path

    def connect(self):
        conn = sqlite3.connect(self.path, isolation_level=None, check_same_thread=False, timeout=30)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA foreign_keys = ON")
        return conn

    def adapt(self, sql: str) -> str:
        return sql.replace("%s", "?")

    def begin(self, conn) -> None:
        conn.execute("BEGIN IMMEDIATE")

    def ping(self, conn) -> None:
        conn.execute("SELECT 1")


def make_database(pool_size: int = 4):
    """Fresh temporary database file with the real migrations applied. Returns (Database, path)."""
    from app import migrate

    fd, path = tempfile.mkstemp(suffix=".sqlite3")
    os.close(fd)
    db = Database(SQLiteBackend(path), pool_size=pool_size, acquire_timeout_s=20)
    migrate.apply_all(db, translate=translate_ddl, log=lambda *_: None)
    return db, path
