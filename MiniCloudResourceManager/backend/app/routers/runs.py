from typing import Optional

from fastapi import APIRouter, Depends, Query

from ..deps import current_user, get_ctx
from ..models import ALGORITHMS, CompareSavedRequest, ComparisonRequest, RunRequest
from ..services import runs as runs_service

router = APIRouter(prefix="/api", tags=["scheduling"])

ALG_PATTERN = "^(" + "|".join(ALGORITHMS) + ")$"


@router.post("/runs", status_code=201)
def create_run(body: RunRequest, ctx=Depends(get_ctx), user=Depends(current_user)):
    return runs_service.run(ctx, user["id"], body.model_dump())


@router.get("/runs")
def list_runs(algorithm: Optional[str] = Query(None, pattern=ALG_PATTERN),
              status: Optional[str] = Query(None, pattern="^(RUNNING|COMPLETED|FAILED)$"),
              comparison_id: Optional[int] = Query(None, ge=1),
              sort: str = Query("id", pattern="^[a-z_]{1,20}$"), order: str = Query("desc", pattern="^(asc|desc)$"),
              page: int = Query(1, ge=1), page_size: int = Query(20, ge=1, le=200),
              ctx=Depends(get_ctx), user=Depends(current_user)):
    return runs_service.list_runs(ctx, user["id"], algorithm=algorithm, status=status, comparison_id=comparison_id,
                                  sort=sort, order=order, page=page, page_size=page_size)


@router.post("/runs/compare-saved")
def compare_saved(body: CompareSavedRequest, ctx=Depends(get_ctx), user=Depends(current_user)):
    return runs_service.compare_saved(ctx, user["id"], body.run_ids)


@router.get("/runs/{run_id}")
def get_run(run_id: int, ctx=Depends(get_ctx), user=Depends(current_user)):
    return runs_service.detail(ctx, user["id"], run_id)


@router.get("/runs/{run_id}/results")
def get_results(run_id: int, ctx=Depends(get_ctx), user=Depends(current_user)):
    return runs_service.results(ctx, user["id"], run_id)


@router.get("/runs/{run_id}/gantt")
def get_gantt(run_id: int, ctx=Depends(get_ctx), user=Depends(current_user)):
    return runs_service.gantt(ctx, user["id"], run_id)


@router.get("/runs/{run_id}/utilization")
def get_utilization(run_id: int, ctx=Depends(get_ctx), user=Depends(current_user)):
    return runs_service.utilization_series(ctx, user["id"], run_id)


@router.delete("/runs/{run_id}", status_code=204)
def delete_run(run_id: int, ctx=Depends(get_ctx), user=Depends(current_user)):
    runs_service.delete_run(ctx, user["id"], run_id)


@router.post("/comparisons", status_code=201)
def create_comparison(body: ComparisonRequest, ctx=Depends(get_ctx), user=Depends(current_user)):
    return runs_service.compare(ctx, user["id"], body.model_dump())


@router.get("/comparisons")
def list_comparisons(page: int = Query(1, ge=1), page_size: int = Query(20, ge=1, le=200),
                     ctx=Depends(get_ctx), user=Depends(current_user)):
    return runs_service.list_comparisons(ctx, user["id"], page, page_size)


@router.get("/comparisons/{comparison_id}")
def get_comparison(comparison_id: int, ctx=Depends(get_ctx), user=Depends(current_user)):
    return runs_service.comparison_detail(ctx, user["id"], comparison_id)
