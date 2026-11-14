"""Shared request helpers: the application context, the logged-in user, and query-parameter parsing."""
from __future__ import annotations

from datetime import datetime

from fastapi import Depends, Request
from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials

bearer_scheme = HTTPBearer(auto_error=False)

from .errors import ApiError, unauthorized, unprocessable
from .services import auth


def get_ctx(request: Request):
    return request.app.state.ctx


def current_user(
    credentials: HTTPAuthorizationCredentials = Depends(bearer_scheme),
    ctx=Depends(get_ctx)
) -> dict:
    """The authenticated user, loaded from the database."""

    if credentials is None:
        raise unauthorized("Missing bearer token")

    return auth.authenticate(ctx, credentials.credentials)


def client_address(request: Request) -> str:
    return request.client.host if request.client else "unknown"


def parse_datetime(value, end_of_day: bool = False):
    """'2026-11-01' or '2026-11-01T10:30:00' -> 'YYYY-MM-DD HH:MM:SS' (UTC), or None."""
    if value is None or value == "":
        return None
    try:
        text = value.strip().rstrip("Z")
        dt = datetime.fromisoformat(text)
    except ValueError as exc:
        raise unprocessable(f"'{value}' is not a valid date (use YYYY-MM-DD or YYYY-MM-DDTHH:MM:SS)") from exc
    if len(text) <= 10 and end_of_day:
        dt = dt.replace(hour=23, minute=59, second=59)
    return dt.strftime("%Y-%m-%d %H:%M:%S")
