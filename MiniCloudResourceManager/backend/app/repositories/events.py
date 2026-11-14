from __future__ import annotations

from ..db import Session, iso
from . import like_pattern, page_args


def present(row: dict) -> dict:
    return {"id": row["id"], "event_type": row["event_type"], "description": row["description"], "run_id": row["run_id"],
            "job_id": row["job_id"], "sim_time": row["sim_time"], "created_at": iso(row["created_at"])}


def add(tx: Session, user_id: int | None, event_type: str, description: str, *, run_id: int | None = None,
        job_id: int | None = None, sim_time: int | None = None) -> int:
    return tx.insert(
        "INSERT INTO system_events (user_id, run_id, job_id, event_type, description, sim_time) VALUES (%s, %s, %s, %s, %s, %s)",
        (user_id, run_id, job_id, event_type, description[:1000], sim_time))


def add_many(tx: Session, rows: list[tuple]) -> None:
    """rows: (user_id, run_id, job_id, event_type, description, sim_time)"""
    tx.execute_many(
        "INSERT INTO system_events (user_id, run_id, job_id, event_type, description, sim_time) VALUES (%s, %s, %s, %s, %s, %s)",
        [(u, r, j, t, d[:1000], s) for (u, r, j, t, d, s) in rows])


def search(tx: Session, user_id: int, *, event_type: str | None, run_id: int | None, job_id: int | None,
           text: str | None, since: str | None, until: str | None, order: str, page: int,
           page_size: int) -> tuple[list[dict], int]:
    where, params = ["user_id = %s"], [user_id]
    if event_type:
        where.append("event_type = %s"); params.append(event_type)
    if run_id is not None:
        where.append("run_id = %s"); params.append(run_id)
    if job_id is not None:
        where.append("job_id = %s"); params.append(job_id)
    if text:
        where.append("description LIKE %s ESCAPE '!'"); params.append(like_pattern(text))
    if since:
        where.append("created_at >= %s"); params.append(since)
    if until:
        where.append("created_at <= %s"); params.append(until)
    clause = " AND ".join(where)
    direction = "ASC" if order.lower() == "asc" else "DESC"
    limit, offset = page_args(page, page_size)
    total = int(tx.scalar(f"SELECT COUNT(*) FROM system_events WHERE {clause}", params) or 0)
    rows = tx.fetch_all(
        "SELECT id, run_id, job_id, event_type, description, sim_time, created_at FROM system_events "
        f"WHERE {clause} ORDER BY id {direction} LIMIT %s OFFSET %s", (*params, limit, offset))
    return rows, total


def event_types(tx: Session, user_id: int) -> list[str]:
    rows = tx.fetch_all("SELECT DISTINCT event_type FROM system_events WHERE user_id = %s ORDER BY event_type", (user_id,))
    return [r["event_type"] for r in rows]
