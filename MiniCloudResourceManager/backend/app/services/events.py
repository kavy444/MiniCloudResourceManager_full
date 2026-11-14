from __future__ import annotations

from ..repositories import events as events_repo


def search(ctx, user_id: int, **filters) -> dict:
    with ctx.db.transaction() as tx:
        rows, total = events_repo.search(tx, user_id, **filters)
        types = events_repo.event_types(tx, user_id)
    return {"items": [events_repo.present(r) for r in rows], "total": total, "page": max(1, filters["page"]),
            "page_size": min(max(1, filters["page_size"]), 200), "event_types": types}
