from __future__ import annotations

from ..db import Session, iso, utcnow_str
from . import page_args

_COLUMNS = ("id, name, total_cpu, total_ram_gb, total_storage_gb, allocated_cpu, allocated_ram_gb, allocated_storage_gb, "
            "updated_at")


def present(row: dict) -> dict:
    def util(used: int, total: int) -> float:
        return round(100.0 * used / total, 2) if total else 0.0

    return {
        "id": row["id"], "name": row["name"],
        "total": {"cpu": row["total_cpu"], "ram_gb": row["total_ram_gb"], "storage_gb": row["total_storage_gb"]},
        "allocated": {"cpu": row["allocated_cpu"], "ram_gb": row["allocated_ram_gb"], "storage_gb": row["allocated_storage_gb"]},
        "available": {"cpu": row["total_cpu"] - row["allocated_cpu"], "ram_gb": row["total_ram_gb"] - row["allocated_ram_gb"],
                      "storage_gb": row["total_storage_gb"] - row["allocated_storage_gb"]},
        "utilization_percent": {"cpu": util(row["allocated_cpu"], row["total_cpu"]),
                                "ram": util(row["allocated_ram_gb"], row["total_ram_gb"]),
                                "storage": util(row["allocated_storage_gb"], row["total_storage_gb"])},
        "updated_at": iso(row["updated_at"]),
    }


def list_all(tx: Session, lock: bool = False) -> list[dict]:
    return tx.fetch_all(f"SELECT {_COLUMNS} FROM resources ORDER BY id" + (tx.lock_suffix if lock else ""))


def get(tx: Session, resource_id: int, lock: bool = False) -> dict | None:
    return tx.fetch_one(f"SELECT {_COLUMNS} FROM resources WHERE id = %s" + (tx.lock_suffix if lock else ""), (resource_id,))


def update_config(tx: Session, resource_id: int, name: str, cpu: int, ram: int, storage: int) -> int:
    return tx.execute("UPDATE resources SET name = %s, total_cpu = %s, total_ram_gb = %s, total_storage_gb = %s, updated_at = %s "
                      "WHERE id = %s", (name, cpu, ram, storage, utcnow_str(), resource_id))


def apply_delta(tx: Session, resource_id: int, cpu: int, ram: int, storage: int) -> int:
    """Add (or, with negative numbers, subtract) allocated amounts. The CHECK constraints reject any result that is
    negative or above capacity, so the database itself refuses to be over-allocated."""
    return tx.execute(
        "UPDATE resources SET allocated_cpu = allocated_cpu + %s, allocated_ram_gb = allocated_ram_gb + %s, "
        "allocated_storage_gb = allocated_storage_gb + %s, updated_at = %s WHERE id = %s",
        (cpu, ram, storage, utcnow_str(), resource_id))


def search_allocations(tx: Session, user_id: int, *, run_id: int | None, job_id: int | None, status: str | None,
                       server: int | None, order: str, page: int, page_size: int) -> tuple[list[dict], int]:
    where = ["r.user_id = %s"]
    params: list = [user_id]
    if run_id is not None:
        where.append("a.run_id = %s"); params.append(run_id)
    if job_id is not None:
        where.append("a.job_id = %s"); params.append(job_id)
    if status:
        where.append("a.alloc_status = %s"); params.append(status)
    if server is not None:
        where.append("a.server_ref = %s"); params.append(server)
    clause = " AND ".join(where)
    direction = "ASC" if order.lower() == "asc" else "DESC"
    limit, offset = page_args(page, page_size)
    base = "FROM resource_allocations a JOIN scheduling_runs r ON r.id = a.run_id"
    total = int(tx.scalar(f"SELECT COUNT(*) {base} WHERE {clause}", params) or 0)
    rows = tx.fetch_all(
        "SELECT a.id, a.run_id, a.job_id, a.process_ref, a.job_name, a.resource_id, a.server_ref, a.cpu_cores, a.ram_gb, "
        f"a.storage_gb, a.allocated_at, a.released_at, a.alloc_status, a.created_at {base} WHERE {clause} "
        f"ORDER BY a.id {direction} LIMIT %s OFFSET %s", (*params, limit, offset))
    return rows, total
