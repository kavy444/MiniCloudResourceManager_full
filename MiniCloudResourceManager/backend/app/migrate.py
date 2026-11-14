"""Tiny migration runner: applies migrations/*.sql in file-name order and records them in `schema_migrations`,
so running it twice never applies a file twice. Each file is executed statement by statement.

Tables are created ONLY by these files (no tables are created from application code)."""
from __future__ import annotations

import re
from pathlib import Path

MIGRATIONS_DIR = Path(__file__).resolve().parent.parent / "migrations"

_TRACKING_TABLE = (
    "CREATE TABLE IF NOT EXISTS schema_migrations ("
    "version VARCHAR(100) NOT NULL PRIMARY KEY, applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP)"
)


def split_statements(sql: str) -> list[str]:
    """Splits a SQL script on ';' (the migration files contain no ';' inside strings) and drops '--' comment lines."""
    lines = [ln for ln in sql.splitlines() if not ln.strip().startswith("--")]
    return [s.strip() for s in "\n".join(lines).split(";") if s.strip()]


def migration_files(directory: Path = MIGRATIONS_DIR) -> list[Path]:
    files = sorted(p for p in directory.glob("*.sql") if re.match(r"^\d{3}_", p.name))
    if not files:
        raise FileNotFoundError(f"no migration files found in {directory}")
    return files


def apply_all(db, directory: Path = MIGRATIONS_DIR, translate=None, log=print) -> list[str]:
    """Applies pending migrations. `translate` lets the tests adapt MySQL DDL for SQLite; production passes None.
    Returns the versions that were applied now."""
    applied_now: list[str] = []
    with db.transaction() as tx:
        tx.execute(translate(_TRACKING_TABLE) if translate else _TRACKING_TABLE)
    for path in migration_files(directory):
        with db.transaction() as tx:
            if tx.fetch_one("SELECT version FROM schema_migrations WHERE version = %s", (path.name,)):
                continue
            for statement in split_statements(path.read_text(encoding="utf-8")):
                tx.execute(translate(statement) if translate else statement)
            tx.execute("INSERT INTO schema_migrations (version) VALUES (%s)", (path.name,))
        applied_now.append(path.name)
        log(f"applied {path.name}")
    return applied_now
