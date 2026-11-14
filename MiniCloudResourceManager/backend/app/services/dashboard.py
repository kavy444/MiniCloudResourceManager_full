from __future__ import annotations

from ..repositories import jobs as jobs_repo
from ..repositories import runs as runs_repo
from . import resources as resources_service


def overview(ctx, user_id: int) -> dict:
    with ctx.db.transaction() as tx:
        counts = jobs_repo.counts_by_status(tx, user_id)
        latest = runs_repo.search(tx, user_id, algorithm=None, status="COMPLETED", comparison_id=None,
                                  sort="id", order="desc", page=1, page_size=1)
        run_total = runs_repo.search(tx, user_id, algorithm=None, status=None, comparison_id=None,
                                     sort="id", order="desc", page=1, page_size=1)[1]
    items, _ = latest
    return {"jobs": {"total": sum(counts.values()), "by_status": counts},
            "runs": {"total": run_total, "latest": runs_repo.present_run(items[0]) if items else None},
            "cluster": resources_service.list_servers(ctx)}
