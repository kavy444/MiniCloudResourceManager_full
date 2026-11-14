"""Optional demo data:   python backend/scripts/seed_demo.py
Creates a demo account and loads the labelled sample jobs (is_sample = true) for it. The password is typed at the
prompt (or read from the DEMO_PASSWORD environment variable); it is never stored in a file or printed."""
import getpass
import os

from _common import open_database

from app.errors import ApiError
from app.engine import EngineClient
from app.security import LoginLimiter
from app.services import Context, auth, jobs


def main() -> None:
    settings, db = open_database()
    ctx = Context(db=db, settings=settings, engine=EngineClient(settings.engine_path), limiter=LoginLimiter())
    username = os.environ.get("DEMO_USERNAME", "demo")
    password = os.environ.get("DEMO_PASSWORD") or getpass.getpass(f"Choose a password for '{username}' (min 8 characters): ")
    if len(password) < 8:
        raise SystemExit("The password must have at least 8 characters.")
    try:
        user = auth.register(ctx, username, password)["user"]
    except ApiError as exc:
        raise SystemExit(f"Could not create the demo user: {exc.message}")
    created = jobs.load_samples(ctx, user["id"])
    print(f"Created user '{username}' (role {user['role']}) with {len(created)} sample jobs.")
    db.close()


if __name__ == "__main__":
    main()
