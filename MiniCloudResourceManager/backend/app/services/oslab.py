"""OS teaching modules (memory allocation, Banker's algorithm / deadlock, synchronisation). The algorithms run in
the C++ engine; deadlock checks are stored in `deadlock_checks`, every module call is logged in `system_events`."""
from __future__ import annotations

from ..errors import unprocessable
from ..repositories import checks as checks_repo
from ..repositories import events as events_repo


def _call(ctx, request: dict) -> dict:
    out = ctx.engine.call(request)
    if not out.get("ok"):
        raise unprocessable("The engine rejected the input", out.get("errors"))
    return out


def memory(ctx, user_id: int, req: dict) -> dict:
    body = {"command": "memory", "strategy": req["strategy"], "operations": req["operations"]}
    for key in ("total_size", "partitions"):
        if req.get(key) is not None:
            body[key] = req[key]
    out = _call(ctx, body)
    with ctx.db.transaction() as tx:
        events_repo.add(tx, user_id, "MEMORY_SIMULATION",
                        f"{req['strategy']} simulation with {len(req['operations'])} operations, "
                        f"{out.get('failed_allocations', 0)} failed allocations")
    return out


def banker(ctx, user_id: int, req: dict) -> dict:
    body = {"command": "banker", "action": req["action"], "available": req["available"],
            "allocation": req["allocation"]}
    for key in ("max", "request", "process"):
        if req.get(key) is not None:
            body[key] = req[key]
    out = _call(ctx, body)
    action = req["action"]
    if action == "safety":
        s = out["safety"]
        check_type, outcome = "SAFETY", "SAFE" if s["safe"] else "UNSAFE"
        seq, text = (",".join(f"P{p}" for p in s["sequence"]) or None), s["explanation"]
    elif action == "request":
        check_type, outcome = "REQUEST", "GRANTED" if out["granted"] else "DENIED"
        safe = out.get("safety_if_granted") or {}
        seq = ",".join(f"P{p}" for p in safe.get("sequence", [])) or None
        text = out["reason"]
    else:
        check_type, outcome = "DETECT", "DEADLOCK" if out["deadlocked"] else "NO_DEADLOCK"
        seq, text = None, out["explanation"]
    with ctx.db.transaction() as tx:
        check_id = checks_repo.add(tx, user_id, check_type, outcome, req, out, seq, text)
        events_repo.add(tx, user_id, "DEADLOCK_CHECK", f"{check_type} check -> {outcome}")
    out["saved_check_id"] = check_id
    return out


def sync(ctx, user_id: int, req: dict) -> dict:
    out = _call(ctx, {"command": "sync", **req})
    with ctx.db.transaction() as tx:
        events_repo.add(tx, user_id, "SYNC_DEMO", f"Synchronisation demo '{req['scenario']}' executed")
    return out


def history(ctx, user_id: int, **filters) -> dict:
    with ctx.db.transaction() as tx:
        rows, total = checks_repo.search(tx, user_id, **filters)
    return {"items": [checks_repo.present(r) for r in rows], "total": total, "page": max(1, filters["page"]),
            "page_size": min(max(1, filters["page_size"]), 200)}
