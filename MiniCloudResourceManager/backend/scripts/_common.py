from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


def open_database():
    """Settings from .env / environment -> Database. Prints a friendly message and exits on configuration problems."""
    from app.config import ConfigError, Settings
    from app.db import Database, MySQLBackend
    try:
        settings = Settings.from_env()
    except ConfigError as exc:
        sys.exit(f"Configuration problem: {exc}\nCopy backend/.env.example to backend/.env and fill in the values.")
    try:
        backend = MySQLBackend(settings)
    except ImportError:
        sys.exit("PyMySQL is not installed. Run:  pip install -r backend/requirements.txt")
    return settings, Database(backend, pool_size=2)
