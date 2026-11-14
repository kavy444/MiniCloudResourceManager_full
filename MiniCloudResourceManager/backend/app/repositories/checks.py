from __future__ import annotations

from ..db import Session, iso
from . import dumps, loads, page_args


def present(row: dict) -> dict:
    return {"id": row["id"], "check_type": row["check_type"], "outcome": row["outcome"],
            "input": loads(row["input_data"]), "result": loads(row["result_data"]),
            "safe_sequence": row["safe_sequence"], "explanation": row["explanation"], "created_at": iso(row["created_at"])}


def add(tx: Session, user_id: int, check_type: str, outcome: str, input_data: dict, result_data: dict,
        safe_sequence: str | None, explanation: str | None) -> int:
    return tx.insert(
        "INSERT INTO deadlock_checks (user_id, check_type, outcome, input_data, result_data, safe_sequence, explanation) "
        "VALUES (%s, %s, %s, %s, %s, %s, %s)",
        (user_id, check_type, outcome, dumps(input_data), dumps(result_data), safe_sequence,
         (explanation or "")[:2000]))


def search(tx: Session, user_id: int, outcome: str | None, order: str, page: int, page_size: int) -> tuple[list[dict], int]:
    where, params = ["user_id = %s"], [user_id]
    if outcome:
        where.append("outcome = %s"); params.append(outcome)
    clause = " AND ".join(where)
    direction = "ASC" if order.lower() == "asc" else "DESC"
    limit, offset = page_args(page, page_size)
    total = int(tx.scalar(f"SELECT COUNT(*) FROM deadlock_checks WHERE {clause}", params) or 0)
    rows = tx.fetch_all(
        "SELECT id, check_type, outcome, input_data, result_data, safe_sequence, explanation, created_at FROM deadlock_checks "
        f"WHERE {clause} ORDER BY id {direction} LIMIT %s OFFSET %s", (*params, limit, offset))
    return rows, total
