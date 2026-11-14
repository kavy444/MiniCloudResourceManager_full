from __future__ import annotations

from ..db import utcnow_str
from ..errors import conflict, unauthorized
from ..repositories import events as events_repo
from ..repositories import users as users_repo
from ..security import DUMMY_HASH, create_token, decode_token, hash_password, verify_password


def _token_response(ctx, user: dict) -> dict:
    token, expires_in = create_token(user["id"], ctx.settings.jwt_secret, ctx.settings.jwt_ttl_minutes)
    return {"access_token": token, "token_type": "bearer", "expires_in": expires_in, "user": users_repo.public(user)}


def register(ctx, username: str, password: str) -> dict:
    password_hash = hash_password(password)          # slow on purpose; done before opening the transaction
    try:
        with ctx.db.transaction() as tx:
            # The first account becomes the administrator (the only one allowed to edit server capacities).
            role = "admin" if users_repo.count(tx) == 0 else "user"
            user_id = users_repo.create(tx, username, password_hash, role)
            events_repo.add(tx, user_id, "USER_REGISTERED", f"User '{username}' registered as {role}")
            user = users_repo.by_id(tx, user_id)
    except ctx.db.integrity_errors as exc:
        raise conflict("That username is already taken") from exc
    return _token_response(ctx, user)


def login(ctx, username: str, password: str, client: str) -> dict:
    key = f"{username.lower()}|{client}"
    ctx.limiter.check(key)
    with ctx.db.transaction() as tx:
        user = users_repo.by_username(tx, username.lower())
    # Always run one scrypt verification so "unknown user" and "wrong password" take the same time.
    ok = verify_password(password, user["password_hash"] if user else DUMMY_HASH)
    if not user or not ok:
        ctx.limiter.record_failure(key)
        raise unauthorized("Incorrect username or password")
    ctx.limiter.reset(key)
    with ctx.db.transaction() as tx:
        users_repo.touch_login(tx, user["id"], utcnow_str())
        events_repo.add(tx, user["id"], "USER_LOGIN", f"User '{user['username']}' logged in")
    return _token_response(ctx, user)


def authenticate(ctx, token: str) -> dict:
    """Token -> current user row (without hash). Reloaded from the DB every time; roles are never read from the token."""
    user_id = decode_token(token, ctx.settings.jwt_secret)
    with ctx.db.transaction() as tx:
        user = users_repo.by_id(tx, user_id)
    if user is None:
        raise unauthorized("Account no longer exists")
    return users_repo.public(user)
