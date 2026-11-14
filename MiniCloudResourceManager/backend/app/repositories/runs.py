from __future__ import annotations

from ..db import Session, iso, utcnow_str
from . import dumps, loads, page_args

RUN_COLUMNS = ("id, user_id, comparison_id, algorithm, params, use_cluster, status, error_message, started_at, completed_at, "
               "total_processes, completed_count, rejected_count, avg_waiting, avg_turnaround, avg_response, throughput, "
               "cpu_utilization, context_switches, preemptions, total_time, first_arrival")
SORTABLE = {"id": "id", "started_at": "started_at", "algorithm": "algorithm", "status": "status",
            "avg_waiting": "avg_waiting", "avg_turnaround": "avg_turnaround", "avg_response": "avg_response",
            "throughput": "throughput", "cpu_utilization": "cpu_utilization", "context_switches": "context_switches"}


def present_run(row: dict | None) -> dict | None:
    if row is None:
        return None
    return {
        "id": row["id"], "comparison_id": row["comparison_id"], "algorithm": row["algorithm"],
        "params": loads(row["params"]), "use_cluster": bool(row["use_cluster"]), "status": row["status"],
        "error_message": row["error_message"], "started_at": iso(row["started_at"]),
        "completed_at": iso(row["completed_at"]),
        "metrics": None if row["completed_count"] is None else {
            "total_processes": row["total_processes"], "completed": row["completed_count"],
            "rejected": row["rejected_count"], "avg_waiting": row["avg_waiting"],
            "avg_turnaround": row["avg_turnaround"], "avg_response": row["avg_response"],
            "throughput": row["throughput"], "cpu_utilization": row["cpu_utilization"],
            "context_switches": row["context_switches"], "preemptions": row["preemptions"],
            "total_time": row["total_time"], "start_time": row["first_arrival"]},
    }


def create(tx: Session, user_id: int, algorithm: str, params: dict, use_cluster: bool, snapshot: dict,
           comparison_id: int | None = None) -> int:
    return tx.insert(
        "INSERT INTO scheduling_runs (user_id, comparison_id, algorithm, params, use_cluster, input_snapshot, status) "
        "VALUES (%s, %s, %s, %s, %s, %s, 'RUNNING')",
        (user_id, comparison_id, algorithm, dumps(params), 1 if use_cluster else 0, dumps(snapshot)))


def mark_failed(tx: Session, run_id: int, message: str) -> None:
    tx.execute("UPDATE scheduling_runs SET status = 'FAILED', error_message = %s, completed_at = %s "
               "WHERE id = %s AND status = 'RUNNING'", (message[:1000], utcnow_str(), run_id))


def mark_completed(tx: Session, run_id: int, m: dict) -> None:
    tx.execute(
        "UPDATE scheduling_runs SET status = 'COMPLETED', error_message = NULL, completed_at = %s, total_processes = %s, "
        "completed_count = %s, rejected_count = %s, avg_waiting = %s, avg_turnaround = %s, avg_response = %s, "
        "throughput = %s, cpu_utilization = %s, context_switches = %s, preemptions = %s, total_time = %s, first_arrival = %s "
        "WHERE id = %s",
        (utcnow_str(), m["total_processes"], m["completed"], m["rejected"], m["avg_waiting"], m["avg_turnaround"],
         m["avg_response"], m["throughput"], m["cpu_utilization"], m["context_switches"], m["preemptions"],
         m["total_time"], m["start_time"], run_id))


def get(tx: Session, user_id: int, run_id: int) -> dict | None:
    return tx.fetch_one(f"SELECT {RUN_COLUMNS} FROM scheduling_runs WHERE id = %s AND user_id = %s", (run_id, user_id))


def get_snapshot(tx: Session, user_id: int, run_id: int):
    row = tx.fetch_one("SELECT input_snapshot FROM scheduling_runs WHERE id = %s AND user_id = %s", (run_id, user_id))
    return None if row is None else loads(row["input_snapshot"])


def get_many(tx: Session, user_id: int, ids: list[int]) -> list[dict]:
    marks = ", ".join(["%s"] * len(ids))
    return tx.fetch_all(f"SELECT {RUN_COLUMNS} FROM scheduling_runs WHERE user_id = %s AND id IN ({marks}) ORDER BY id",
                        (user_id, *ids))


def search(tx: Session, user_id: int, *, algorithm: str | None, status: str | None, comparison_id: int | None,
           sort: str, order: str, page: int, page_size: int) -> tuple[list[dict], int]:
    where, params = ["user_id = %s"], [user_id]
    if algorithm:
        where.append("algorithm = %s")
        params.append(algorithm)
    if status:
        where.append("status = %s")
        params.append(status)
    if comparison_id is not None:
        where.append("comparison_id = %s")
        params.append(comparison_id)
    clause = " AND ".join(where)
    column = SORTABLE.get(sort, "id")
    direction = "DESC" if order.lower() == "desc" else "ASC"
    limit, offset = page_args(page, page_size)
    total = int(tx.scalar(f"SELECT COUNT(*) FROM scheduling_runs WHERE {clause}", params) or 0)
    rows = tx.fetch_all(
        f"SELECT {RUN_COLUMNS} FROM scheduling_runs WHERE {clause} ORDER BY {column} {direction}, id DESC LIMIT %s OFFSET %s",
        (*params, limit, offset))
    return rows, total


def delete(tx: Session, user_id: int, run_id: int) -> int:
    return tx.execute("DELETE FROM scheduling_runs WHERE id = %s AND user_id = %s", (run_id, user_id))


# ---- details ----------------------------------------------------------------------------------------------
def insert_results(tx: Session, run_id: int, rows: list[tuple]) -> None:
    tx.execute_many(
        "INSERT INTO scheduling_results (run_id, job_id, process_ref, job_name, arrival_time, burst_time, priority, "
        "result_status, reason, server_ref, admitted_at, first_start, completion_time, turnaround_time, waiting_time, "
        "response_time) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)",
        [(run_id, *r) for r in rows])


def insert_segments(tx: Session, run_id: int, rows: list[tuple]) -> None:
    tx.execute_many(
        "INSERT INTO execution_segments (run_id, seq, process_ref, job_name, server_ref, start_time, end_time, "
        "segment_type, end_reason, queue_level) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s)",
        [(run_id, *r) for r in rows])


def insert_allocations(tx: Session, run_id: int, rows: list[tuple]) -> None:
    tx.execute_many(
        "INSERT INTO resource_allocations (run_id, job_id, process_ref, job_name, resource_id, server_ref, cpu_cores, "
        "ram_gb, storage_gb, allocated_at, released_at, alloc_status) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)",
        [(run_id, *r) for r in rows])


def results(tx: Session, run_id: int) -> list[dict]:
    rows = tx.fetch_all(
        "SELECT job_id, process_ref, job_name, arrival_time, burst_time, priority, result_status, reason, server_ref, "
        "admitted_at, first_start, completion_time, turnaround_time, waiting_time, response_time "
        "FROM scheduling_results WHERE run_id = %s ORDER BY process_ref", (run_id,))
    return rows


def segments(tx: Session, run_id: int) -> list[dict]:
    return tx.fetch_all(
        "SELECT seq, process_ref, job_name, server_ref, start_time, end_time, segment_type, end_reason, queue_level "
        "FROM execution_segments WHERE run_id = %s ORDER BY seq", (run_id,))


def allocations_for_run(tx: Session, run_id: int) -> list[dict]:
    return tx.fetch_all(
        "SELECT id, run_id, job_id, process_ref, job_name, resource_id, server_ref, cpu_cores, ram_gb, storage_gb, "
        "allocated_at, released_at, alloc_status FROM resource_allocations WHERE run_id = %s ORDER BY id", (run_id,))


# ---- comparisons --------------------------------------------------------------------------------------------
def create_comparison(tx: Session, user_id: int, name: str, algorithms: list[str], params: dict) -> int:
    return tx.insert("INSERT INTO algorithm_comparisons (user_id, name, algorithms, params) VALUES (%s, %s, %s, %s)",
                     (user_id, name, dumps(algorithms), dumps(params)))


def set_comparison_best(tx: Session, comparison_id: int, best: list) -> None:
    tx.execute("UPDATE algorithm_comparisons SET best_by_metric = %s WHERE id = %s", (dumps(best), comparison_id))


def present_comparison(row: dict | None) -> dict | None:
    if row is None:
        return None
    return {"id": row["id"], "name": row["name"], "algorithms": loads(row["algorithms"]),
            "params": loads(row["params"]), "best_by_metric": loads(row["best_by_metric"]),
            "created_at": iso(row["created_at"])}


def get_comparison(tx: Session, user_id: int, comparison_id: int) -> dict | None:
    return tx.fetch_one("SELECT id, name, algorithms, params, best_by_metric, created_at FROM algorithm_comparisons "
                        "WHERE id = %s AND user_id = %s", (comparison_id, user_id))


def list_comparisons(tx: Session, user_id: int, page: int, page_size: int) -> tuple[list[dict], int]:
    limit, offset = page_args(page, page_size)
    total = int(tx.scalar("SELECT COUNT(*) FROM algorithm_comparisons WHERE user_id = %s", (user_id,)) or 0)
    rows = tx.fetch_all("SELECT id, name, algorithms, params, best_by_metric, created_at FROM algorithm_comparisons "
                        "WHERE user_id = %s ORDER BY id DESC LIMIT %s OFFSET %s", (user_id, limit, offset))
    return rows, total
