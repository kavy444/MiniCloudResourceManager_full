"""Business logic. Services own transactions; routers only translate HTTP <-> service calls."""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass
class Context:
    """Everything a service needs. Built once in main.create_app (and by the tests)."""
    db: Any          # app.db.Database
    settings: Any    # app.config.Settings
    engine: Any      # app.engine.EngineClient
    limiter: Any     # app.security.LoginLimiter
