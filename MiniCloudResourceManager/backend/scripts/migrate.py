"""Create/upgrade the tables:   python backend/scripts/migrate.py
Applies backend/migrations/*.sql in order (already-applied files are skipped), including the seed data for the
three servers. Uses the application account from .env; it needs CREATE/ALTER/INDEX/INSERT rights on the database."""
from _common import open_database

from app import migrate
from app.errors import ApiError


def main() -> None:
    settings, db = open_database()
    print(f"Database: {settings.db_name} on {settings.db_host}:{settings.db_port} as {settings.db_user}")
    try:
        applied = migrate.apply_all(db)
    except ApiError as exc:
        raise SystemExit(f"Could not reach MySQL: {exc.message}")
    except Exception as exc:       # noqa: BLE001 - show the SQL error, never the password
        raise SystemExit(f"Migration failed: {exc}")
    print("Nothing to do - schema is up to date." if not applied else f"Done: {len(applied)} migration(s) applied.")
    db.close()


if __name__ == "__main__":
    main()
