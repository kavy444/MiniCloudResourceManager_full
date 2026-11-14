"""SQL lives here and only here. Every function takes the open transaction `tx` (a db.Session) and uses
%s parameters for all values. Sort columns come from fixed whitelists, never from user text."""
from __future__ import annotations

import json
from typing import Any


def dumps(value: Any) -> str:
    return json.dumps(value, separators=(",", ":"), sort_keys=False)


def loads(value: Any) -> Any:
    """JSON columns come back as text from some drivers and as parsed objects from others."""
    if value is None:
        return None
    if isinstance(value, (bytes, bytearray)):
        value = value.decode("utf-8")
    if isinstance(value, str):
        return json.loads(value)
    return value


def page_args(page: int, page_size: int) -> tuple[int, int]:
    page = max(1, int(page))
    page_size = min(max(1, int(page_size)), 200)
    return page_size, (page - 1) * page_size


def like_pattern(text: str) -> str:
    """Escape LIKE wildcards in user text and wrap it for a 'contains' search. Use together with ESCAPE '!'
    ('!' is used because a backslash would be interpreted differently by different SQL dialects)."""
    escaped = text.replace("!", "!!").replace("%", "!%").replace("_", "!_")
    return f"%{escaped}%"
