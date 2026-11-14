from __future__ import annotations

from ..db import Session, iso, utcnow_str
from . import like_pattern, page_args

_COLUMNS = ("id, user_id, name, arrival_time, burst_time, priority, queue_level, cpu_cores, ram_gb, storage_gb, "
            "status, status_reason, is_sample, created_at, updated_at")

SORTABLE = {"id": "id", "name": "name", "arrival_time": "arrival_time", "burst_time": "burst_time",
            "priority": "priority", "status": "status", "created_at": "created_at", "updated_at": "updated_at"}


def present(row: dict | None) -> dict | None:
    if row is None:
        return None
    return {
        "id": row["id"], "name": row["name"], "arrival_time": row["arrival_time"], "burst_time": row["burst_time"],
        "priority": row["priority"], "queue_level": row["queue_level"], "cpu_cores": row["cpu_cores"],
        "ram_gb": row["ram_gb"], "storage_gb": row["storage_gb"], "status": row["status"],
        "status_reason": row["status_reason"], "is_sample": bool(row["is_sample"]),
        "created_at": iso(row["created_at"]), "updated_at": iso(row["updated_at"]),
    }


def insert(tx: Session, user_id: int, data: dict, is_sample: bool = False) -> int:
    return tx.insert(
        "INSERT INTO jobs (user_id, name, arrival_time, burst_time, priority, queue_level, cpu_cores, ram_gb, "
        "storage_gb, status, is_sample) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, 'NEW', %s)",
        (user_id, data["name"], data["arrival_time"], data["burst_time"], data["priority"], data.get("queue_level"),
         data["cpu_cores"], data["ram_gb"], data["storage_gb"], 1 if is_sample else 0))


def get(tx: Session, user_id: int, job_id: int, lock: bool = False) -> dict | None:
    sql = f"SELECT {_COLUMNS} FROM jobs WHERE id = %s AND user_id = %s"   # user_id in the WHERE = ownership check
    return tx.fetch_one(sql + (tx.lock_suffix if lock else ""), (job_id, user_id))


def get_many(tx: Session, user_id: int, ids: list[int], lock: bool = False) -> list[dict]:
    if not ids:
        return []
    marks = ", ".join(["%s"] * len(ids))
    sql = f"SELECT {_COLUMNS} FROM jobs WHERE user_id = %s AND id IN ({marks}) ORDER BY id"
    return tx.fetch_all(sql + (tx.lock_suffix if lock else ""), (user_id, *ids))


def schedulable(tx: Session, user_id: int, lock: bool = False) -> list[dict]:
    sql = (f"SELECT {_COLUMNS} FROM jobs WHERE user_id = %s AND status <> 'CANCELLED' AND status <> 'RUNNING' ORDER BY id")
    return tx.fetch_all(sql + (tx.lock_suffix if lock else ""), (user_id,))


def search(tx: Session, user_id: int, *, status: str | None, text: str | None, sort: str, order: str,
           page: int, page_size: int) -> tuple[list[dict], int]:
    where, params = ["user_id = %s"], [user_id]
    if status:
        where.append("status = %s")
        params.append(status)
    if text:
        where.append("name LIKE %s ESCAPE '!'")
        params.append(like_pattern(text))
    clause = " AND ".join(where)
    column = SORTABLE.get(sort, "id")
    direction = "DESC" if order.lower() == "desc" else "ASC"
    limit, offset = page_args(page, page_size)
    total = int(tx.scalar(f"SELECT COUNT(*) FROM jobs WHERE {clause}", params) or 0)
    rows = tx.fetch_all(f"SELECT {_COLUMNS} FROM jobs WHERE {clause} ORDER BY {column} {direction}, id ASC LIMIT %s OFFSET %s",
                        (*params, limit, offset))
    return rows, total


def update_fields(tx: Session, user_id: int, job_id: int, data: dict, status: str, reason: str | None) -> int:
    return tx.execute(
        "UPDATE jobs SET name = %s, arrival_time = %s, burst_time = %s, priority = %s, queue_level = %s, "
        "cpu_cores = %s, ram_gb = %s, storage_gb = %s, status = %s, status_reason = %s, updated_at = %s "
        "WHERE id = %s AND user_id = %s",
        (data["name"], data["arrival_time"], data["burst_time"], data["priority"], data.get("queue_level"),
         data["cpu_cores"], data["ram_gb"], data["storage_gb"], status, reason, utcnow_str(), job_id, user_id))


def set_status(tx: Session, user_id: int, job_id: int, status: str, reason: str | None) -> int:
    return tx.execute("UPDATE jobs SET status = %s, status_reason = %s, updated_at = %s WHERE id = %s AND user_id = %s",
                      (status, reason, utcnow_str(), job_id, user_id))


def delete(tx: Session, user_id: int, job_id: int) -> int:
    return tx.execute("DELETE FROM jobs WHERE id = %s AND user_id = %s", (job_id, user_id))


def counts_by_status(tx: Session, user_id: int) -> dict:
    rows = tx.fetch_all("SELECT status, COUNT(*) AS n FROM jobs WHERE user_id = %s GROUP BY status", (user_id,))
    return {r["status"]: int(r["n"]) for r in rows}
