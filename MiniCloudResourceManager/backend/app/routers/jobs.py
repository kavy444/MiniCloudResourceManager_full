from typing import Optional

from fastapi import APIRouter, Depends, Query

from ..deps import current_user, get_ctx
from ..models import JobIn
from ..services import jobs as jobs_service

router = APIRouter(prefix="/api/jobs", tags=["jobs"])

STATUS_PATTERN = "^(NEW|READY|RUNNING|WAITING|COMPLETED|FAILED|CANCELLED)$"


@router.get("")
def list_jobs(status: Optional[str] = Query(None, pattern=STATUS_PATTERN), q: Optional[str] = Query(None, max_length=100),
              sort: str = Query("id", pattern="^[a-z_]{1,20}$"), order: str = Query("asc", pattern="^(asc|desc)$"),
              page: int = Query(1, ge=1), page_size: int = Query(50, ge=1, le=200),
              ctx=Depends(get_ctx), user=Depends(current_user)):
    return jobs_service.list_jobs(ctx, user["id"], status=status, text=q, sort=sort, order=order, page=page,
                                  page_size=page_size)


@router.post("", status_code=201)
def create_job(body: JobIn, ctx=Depends(get_ctx), user=Depends(current_user)):
    return jobs_service.create(ctx, user["id"], body.model_dump())


@router.post("/samples", status_code=201)
def load_samples(ctx=Depends(get_ctx), user=Depends(current_user)):
    return {"items": jobs_service.load_samples(ctx, user["id"]), "note": "Sample jobs are flagged is_sample = true"}


@router.get("/{job_id}")
def get_job(job_id: int, ctx=Depends(get_ctx), user=Depends(current_user)):
    return jobs_service.get(ctx, user["id"], job_id)


@router.put("/{job_id}")
def update_job(job_id: int, body: JobIn, ctx=Depends(get_ctx), user=Depends(current_user)):
    return jobs_service.update(ctx, user["id"], job_id, body.model_dump())


@router.post("/{job_id}/cancel")
def cancel_job(job_id: int, ctx=Depends(get_ctx), user=Depends(current_user)):
    return jobs_service.cancel(ctx, user["id"], job_id)


@router.delete("/{job_id}", status_code=204)
def delete_job(job_id: int, ctx=Depends(get_ctx), user=Depends(current_user)):
    jobs_service.delete(ctx, user["id"], job_id)
