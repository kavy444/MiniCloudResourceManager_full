from fastapi import APIRouter, Depends, Request

from ..deps import client_address, current_user, get_ctx
from ..models import Credentials, LoginRequest
from ..services import auth as auth_service

router = APIRouter(prefix="/api/auth", tags=["auth"])


@router.post("/register", status_code=201)
def register(body: Credentials, ctx=Depends(get_ctx)):
    return auth_service.register(ctx, body.username, body.password)


@router.post("/login")
def login(body: LoginRequest, request: Request, ctx=Depends(get_ctx)):
    return auth_service.login(ctx, body.username, body.password, client_address(request))


@router.get("/me")
def me(user=Depends(current_user)):
    return {"user": user}
