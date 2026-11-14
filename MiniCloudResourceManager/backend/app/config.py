"""Configuration: everything comes from environment variables (optionally loaded from a local .env file).
Secrets (DB password, JWT secret) are never hard-coded and never sent to the browser."""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
BACKEND_DIR = PROJECT_ROOT / "backend"


class ConfigError(RuntimeError):
    pass


def load_dotenv_file() -> None:
    """Load backend/.env or <project>/.env if python-dotenv is installed. Real environment variables win."""
    try:
        from dotenv import load_dotenv
    except ImportError:  # python-dotenv is optional: plain environment variables still work
        return
    for candidate in (BACKEND_DIR / ".env", PROJECT_ROOT / ".env"):
        if candidate.is_file():
            load_dotenv(candidate, override=False)


def _int(env: dict, key: str, default: int, minimum: int, maximum: int) -> int:
    raw = env.get(key)
    if raw is None or raw == "":
        return default
    try:
        value = int(raw)
    except ValueError as exc:
        raise ConfigError(f"{key} must be a whole number (got {raw!r})") from exc
    if not minimum <= value <= maximum:
        raise ConfigError(f"{key} must be between {minimum} and {maximum}")
    return value


def _resolve_path(value: str | None) -> str:
    """Relative paths in .env are relative to the project folder (not to wherever the server was started from)."""
    if not value:
        return ""
    path = Path(value)
    return str(path if path.is_absolute() else (PROJECT_ROOT / path).resolve())


def default_engine_path() -> str:
    exe = "mcrm_engine.exe" if os.name == "nt" else "mcrm_engine"
    return str(PROJECT_ROOT / "build" / exe)


@dataclass(frozen=True)
class Settings:
    db_host: str = "127.0.0.1"
    db_port: int = 3306
    db_name: str = "mini_cloud_manager"
    db_user: str = "cloud_app"
    db_password: str = field(default="", repr=False)   # repr=False: never print the password by accident
    db_pool_size: int = 5
    jwt_secret: str = field(default="", repr=False)
    jwt_ttl_minutes: int = 480
    engine_path: str = ""
    engine_timeout_s: int = 20
    engine_max_parallel: int = 4
    cors_origins: tuple = ()
    serve_frontend: bool = True
    frontend_dir: str = str(PROJECT_ROOT / "frontend")
    app_env: str = "development"

    @staticmethod
    def from_env(env: dict | None = None, *, require_secrets: bool = True) -> "Settings":
        if env is None:
            load_dotenv_file()
            env = dict(os.environ)
        secret = env.get("JWT_SECRET", "")
        if require_secrets and len(secret) < 32:
            raise ConfigError(
                "JWT_SECRET must be set to a random string of at least 32 characters "
                '(generate one with: python -c "import secrets; print(secrets.token_urlsafe(48))")'
            )
        if require_secrets and not env.get("DB_PASSWORD"):
            raise ConfigError("DB_PASSWORD is not set. Copy .env.example to .env and fill in your local MySQL password.")
        origins = tuple(o.strip() for o in env.get("CORS_ORIGINS", "").split(",") if o.strip())
        if "*" in origins:
            raise ConfigError("CORS_ORIGINS must list explicit origins, not '*'")
        return Settings(
            db_host=env.get("DB_HOST", "127.0.0.1"),
            db_port=_int(env, "DB_PORT", 3306, 1, 65535),
            db_name=env.get("DB_NAME", "mini_cloud_manager"),
            db_user=env.get("DB_USER", "cloud_app"),
            db_password=env.get("DB_PASSWORD", ""),
            db_pool_size=_int(env, "DB_POOL_SIZE", 5, 1, 50),
            jwt_secret=secret,
            jwt_ttl_minutes=_int(env, "JWT_TTL_MINUTES", 480, 5, 60 * 24 * 7),
            engine_path=_resolve_path(env.get("ENGINE_PATH")) or default_engine_path(),
            engine_timeout_s=_int(env, "ENGINE_TIMEOUT_S", 20, 1, 300),
            engine_max_parallel=_int(env, "ENGINE_MAX_PARALLEL", 4, 1, 32),
            cors_origins=origins,
            serve_frontend=env.get("SERVE_FRONTEND", "1") not in ("0", "false", "False"),
            frontend_dir=env.get("FRONTEND_DIR") or str(PROJECT_ROOT / "frontend"),
            app_env=env.get("APP_ENV", "development"),
        )
