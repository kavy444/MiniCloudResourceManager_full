from typing import Optional

from fastapi import APIRouter, Depends, Query

from ..deps import current_user, get_ctx, parse_datetime
from ..models import ALGORITHMS, BankerRequest, MemoryRequest, ResourceUpdate, SyncRequest
from ..services import dashboard, events as events_service, oslab, resources as resources_service

router = APIRouter(prefix="/api", tags=["system"])

ALGORITHM_INFO = {
    "FCFS": "First Come First Served - non-preemptive, ordered by arrival time (ties: job id).",
    "SJF": "Shortest Job First - non-preemptive, shortest burst first (ties: arrival, then id).",
    "PRIORITY_NP": "Priority, non-preemptive - smaller number = higher priority (ties: arrival, then id).",
    "HRRN": "Highest Response Ratio Next - non-preemptive, ratio (waiting + burst) / burst.",
    "SRTF": "Shortest Remaining Time First - preemptive SJF.",
    "RR": "Round Robin - preemptive, configurable time quantum.",
    "PRIORITY_P": "Priority, preemptive - a more urgent arrival preempts the running job.",
    "MLQ": "Multilevel Queue - fixed queues, each with its own policy (configurable); strict priority between queues.",
    "MLFQ": "Multilevel Feedback Queue - jobs are demoted after using their quantum; optional aging promotes waiting jobs.",
}


@router.get("/health")
def health(ctx=Depends(get_ctx)):
    db_ok = ctx.db.ping()
    engine_ok = ctx.engine.available()
    return {"status": "ok" if db_ok and engine_ok else "degraded", "database": "up" if db_ok else "down",
            "engine": "found" if engine_ok else "missing"}


@router.get("/meta/algorithms")
def algorithms(user=Depends(current_user)):
    return {"items": [{"id": a, "description": ALGORITHM_INFO[a]} for a in ALGORITHMS]}


@router.get("/dashboard")
def overview(ctx=Depends(get_ctx), user=Depends(current_user)):
    return dashboard.overview(ctx, user["id"])


@router.get("/resources")
def list_resources(ctx=Depends(get_ctx), user=Depends(current_user)):
    return resources_service.list_servers(ctx)


@router.put("/resources/{resource_id}")
def update_resource(resource_id: int, body: ResourceUpdate, ctx=Depends(get_ctx), user=Depends(current_user)):
    return resources_service.update_server(ctx, user, resource_id, body.model_dump())


@router.get("/allocations")
def allocation_history(run_id: Optional[int] = Query(None, ge=1), job_id: Optional[int] = Query(None, ge=1),
                       status: Optional[str] = Query(None, pattern="^(ALLOCATED|RELEASED)$"),
                       server: Optional[int] = Query(None, ge=0), order: str = Query("desc", pattern="^(asc|desc)$"),
                       page: int = Query(1, ge=1), page_size: int = Query(50, ge=1, le=200),
                       ctx=Depends(get_ctx), user=Depends(current_user)):
    return resources_service.allocation_history(ctx, user["id"], run_id=run_id, job_id=job_id, status=status,
                                                server=server, order=order, page=page, page_size=page_size)


@router.get("/events")
def list_events(event_type: Optional[str] = Query(None, max_length=40, pattern="^[A-Z_]+$"),
                run_id: Optional[int] = Query(None, ge=1), job_id: Optional[int] = Query(None, ge=1),
                q: Optional[str] = Query(None, max_length=100), since: Optional[str] = Query(None, max_length=30),
                until: Optional[str] = Query(None, max_length=30), order: str = Query("desc", pattern="^(asc|desc)$"),
                page: int = Query(1, ge=1), page_size: int = Query(50, ge=1, le=200),
                ctx=Depends(get_ctx), user=Depends(current_user)):
    return events_service.search(ctx, user["id"], event_type=event_type, run_id=run_id, job_id=job_id, text=q,
                                 since=parse_datetime(since), until=parse_datetime(until, end_of_day=True),
                                 order=order, page=page, page_size=page_size)


@router.post("/os/memory")
def memory(body: MemoryRequest, ctx=Depends(get_ctx), user=Depends(current_user)):
    return oslab.memory(ctx, user["id"], body.model_dump())


@router.post("/os/banker")
def banker(body: BankerRequest, ctx=Depends(get_ctx), user=Depends(current_user)):
    return oslab.banker(ctx, user["id"], body.model_dump())


@router.post("/os/sync")
def sync(body: SyncRequest, ctx=Depends(get_ctx), user=Depends(current_user)):
    return oslab.sync(ctx, user["id"], body.model_dump())


@router.get("/os/deadlock-checks")
def deadlock_checks(outcome: Optional[str] = Query(None, pattern="^(SAFE|UNSAFE|GRANTED|DENIED|DEADLOCK|NO_DEADLOCK)$"),
                    order: str = Query("desc", pattern="^(asc|desc)$"), page: int = Query(1, ge=1),
                    page_size: int = Query(20, ge=1, le=200), ctx=Depends(get_ctx), user=Depends(current_user)):
    return oslab.history(ctx, user["id"], outcome=outcome, order=order, page=page, page_size=page_size)
