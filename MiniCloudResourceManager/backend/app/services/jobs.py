from __future__ import annotations

from .. import states
from ..errors import conflict, not_found
from ..repositories import events as events_repo
from ..repositories import jobs as jobs_repo

SAMPLE_JOBS = [
    # name, arrival, burst, priority(1 = highest), cpu, ram_gb, storage_gb
    ("web-frontend", 0, 6, 2, 2, 4, 20),
    ("batch-etl", 1, 9, 3, 4, 16, 200),
    ("ml-training", 2, 12, 1, 8, 32, 400),
    ("log-compactor", 3, 4, 3, 1, 2, 50),
    ("db-backup", 4, 7, 2, 2, 8, 300),
    ("api-gateway", 5, 3, 1, 2, 4, 10),
    ("report-builder", 6, 5, 2, 4, 12, 80),
    ("video-transcode", 7, 10, 3, 6, 24, 150),
]


def create(ctx, user_id: int, data: dict) -> dict:
    with ctx.db.transaction() as tx:
        job_id = jobs_repo.insert(tx, user_id, data)
        events_repo.add(tx, user_id, "JOB_CREATED", f"Job '{data['name']}' created", job_id=job_id)
        return jobs_repo.present(jobs_repo.get(tx, user_id, job_id))


def get(ctx, user_id: int, job_id: int) -> dict:
    with ctx.db.transaction() as tx:
        row = jobs_repo.get(tx, user_id, job_id)
    if row is None:
        raise not_found("Job")
    return jobs_repo.present(row)


def list_jobs(ctx, user_id: int, **filters) -> dict:
    with ctx.db.transaction() as tx:
        rows, total = jobs_repo.search(tx, user_id, **filters)
        counts = jobs_repo.counts_by_status(tx, user_id)
    return {"items": [jobs_repo.present(r) for r in rows], "total": total,
            "page": max(1, filters["page"]), "page_size": min(max(1, filters["page_size"]), 200), "counts": counts}


def update(ctx, user_id: int, job_id: int, data: dict) -> dict:
    with ctx.db.transaction() as tx:
        row = jobs_repo.get(tx, user_id, job_id, lock=True)
        if row is None:
            raise not_found("Job")
        if row["status"] == states.CANCELLED:
            raise conflict("A cancelled job cannot be edited")
        if row["status"] == states.RUNNING:
            raise conflict("A running job cannot be edited")
        # Editing makes earlier results stale, so the job goes back to NEW.
        target = states.NEW if row["status"] != states.NEW else states.NEW
        if row["status"] != states.NEW:
            states.assert_transition(row["status"], target)
        jobs_repo.update_fields(tx, user_id, job_id, data, target, None)
        events_repo.add(tx, user_id, "JOB_UPDATED", f"Job '{data['name']}' edited (was {row['status']}, now NEW)", job_id=job_id)
        return jobs_repo.present(jobs_repo.get(tx, user_id, job_id))


def cancel(ctx, user_id: int, job_id: int) -> dict:
    with ctx.db.transaction() as tx:
        row = jobs_repo.get(tx, user_id, job_id, lock=True)
        if row is None:
            raise not_found("Job")
        if row["status"] not in states.CANCELLABLE:
            raise conflict(f"A job in state {row['status']} cannot be cancelled")
        states.assert_transition(row["status"], states.CANCELLED)
        jobs_repo.set_status(tx, user_id, job_id, states.CANCELLED, "Cancelled by the user")
        events_repo.add(tx, user_id, "JOB_CANCELLED", f"Job '{row['name']}' cancelled", job_id=job_id)
        return jobs_repo.present(jobs_repo.get(tx, user_id, job_id))


def delete(ctx, user_id: int, job_id: int) -> None:
    with ctx.db.transaction() as tx:
        row = jobs_repo.get(tx, user_id, job_id, lock=True)
        if row is None:
            raise not_found("Job")
        if row["status"] == states.RUNNING:
            raise conflict("A running job cannot be deleted")
        events_repo.add(tx, user_id, "JOB_DELETED", f"Job '{row['name']}' deleted", job_id=job_id)
        jobs_repo.delete(tx, user_id, job_id)   # history keeps snapshots; FKs are ON DELETE SET NULL


def load_samples(ctx, user_id: int) -> list[dict]:
    """Adds the clearly flagged sample workload (is_sample = true) to this user's jobs."""
    created = []
    with ctx.db.transaction() as tx:
        for name, arrival, burst, priority, cpu, ram, storage in SAMPLE_JOBS:
            data = {"name": name, "arrival_time": arrival, "burst_time": burst, "priority": priority,
                    "queue_level": None, "cpu_cores": cpu, "ram_gb": ram, "storage_gb": storage}
            job_id = jobs_repo.insert(tx, user_id, data, is_sample=True)
            created.append(job_id)
        events_repo.add(tx, user_id, "SAMPLES_LOADED", f"{len(created)} sample jobs added")
        rows = jobs_repo.get_many(tx, user_id, created)
    return [jobs_repo.present(r) for r in rows]
