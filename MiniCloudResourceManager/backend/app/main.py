"""FastAPI application factory.  Start:  uvicorn app.main:app --port 8000   (from the backend/ folder)"""
from __future__ import annotations

import logging

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from starlette.middleware.cors import CORSMiddleware

from .config import Settings
from .db import Database, MySQLBackend
from .engine import EngineClient
from .errors import ApiError
from .routers import auth, jobs, runs, system
from .security import LoginLimiter
from .services import Context

log = logging.getLogger("mcrm")


def build_context(settings: Settings) -> Context:
    db = Database(MySQLBackend(settings), pool_size=settings.db_pool_size)
    return Context(db=db, settings=settings,
                   engine=EngineClient(settings.engine_path, settings.engine_timeout_s, settings.engine_max_parallel),
                   limiter=LoginLimiter())


def create_app(ctx: Context) -> FastAPI:
    s = ctx.settings
    app = FastAPI(title="Mini Cloud Resource Manager API", version="1.0.0",
                  docs_url="/api/docs", openapi_url="/api/openapi.json", redoc_url=None)
    app.state.ctx = ctx

    if s.cors_origins:       # only the explicitly configured origins; the frontend is normally served from the same origin
        app.add_middleware(CORSMiddleware, allow_origins=list(s.cors_origins), allow_credentials=False,
                           allow_methods=["GET", "POST", "PUT", "DELETE"], allow_headers=["Authorization", "Content-Type"])

    @app.middleware("http")
    async def security_headers(request: Request, call_next):
        response = await call_next(request)
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["X-Frame-Options"] = "DENY"
        response.headers["Referrer-Policy"] = "no-referrer"
        if request.url.path.startswith("/api"):
            response.headers["Cache-Control"] = "no-store"
        return response

    @app.exception_handler(ApiError)
    async def api_error_handler(request: Request, exc: ApiError):
        return JSONResponse(status_code=exc.status_code, content=exc.body())

    @app.exception_handler(RequestValidationError)
    async def validation_handler(request: Request, exc: RequestValidationError):
        details = [{"field": ".".join(str(p) for p in e.get("loc", []) if p != "body"), "message": str(e.get("msg", ""))}
                   for e in exc.errors()]
        return JSONResponse(status_code=422, content={"error": {"code": "validation_error",
                                                                "message": "The request is invalid", "details": details}})

    @app.exception_handler(Exception)
    async def unexpected_handler(request: Request, exc: Exception):
        log.exception("unhandled error on %s %s", request.method, request.url.path)   # details stay in the server log
        return JSONResponse(status_code=500, content={"error": {"code": "internal_error", "message": "Internal server error"}})

    for module in (auth, jobs, runs, system):
        app.include_router(module.router)

    if s.serve_frontend:
        import os
        from starlette.staticfiles import StaticFiles
        if os.path.isdir(s.frontend_dir):
            app.mount("/", StaticFiles(directory=s.frontend_dir, html=True), name="frontend")
    return app


def _default_app() -> FastAPI:
    settings = Settings.from_env()
    return create_app(build_context(settings))


def __getattr__(name):            # `uvicorn app.main:app` builds the app lazily so importing this module has no side effects
    if name == "app":
        application = _default_app()
        globals()["app"] = application
        return application
    raise AttributeError(name)
