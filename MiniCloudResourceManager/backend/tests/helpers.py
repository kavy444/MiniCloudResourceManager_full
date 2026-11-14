from __future__ import annotations

import os
import unittest
from pathlib import Path

from app.config import Settings
from app.engine import EngineClient
from app.security import LoginLimiter
from app.services import Context

ROOT = Path(__file__).resolve().parents[2]
ENGINE_PATH = os.environ.get("ENGINE_PATH") or str(ROOT / "build" / ("mcrm_engine.exe" if os.name == "nt" else "mcrm_engine"))


def engine_available() -> bool:
    return EngineClient(ENGINE_PATH).available()


def make_context(db) -> Context:
    settings = Settings(jwt_secret="t" * 48, engine_path=ENGINE_PATH, cors_origins=("http://localhost:8000",))
    return Context(db=db, settings=settings, engine=EngineClient(ENGINE_PATH, timeout_s=30, max_parallel=4),
                   limiter=LoginLimiter(max_failures=3, window_s=60))


def job(name="j", arrival=0, burst=3, priority=2, cpu=1, ram=1, storage=1, queue=None):
    return {"name": name, "arrival_time": arrival, "burst_time": burst, "priority": priority, "queue_level": queue,
            "cpu_cores": cpu, "ram_gb": ram, "storage_gb": storage}


def run_params(**over):
    base = {"time_quantum": 2, "mlq_queues": None, "mlfq_quanta": None, "mlfq_aging": 0, "use_cluster": True, "job_ids": None}
    base.update(over)
    return base


class DbTestCase(unittest.TestCase):
    def setUp(self):
        from tests.sqlite_backend import make_database
        self.db, self.path = make_database()
        self.ctx = make_context(self.db)

    def tearDown(self):
        self.db.close()
        try:
            os.unlink(self.path)
        except OSError:
            pass

    def user(self, name="alice"):
        from app.services import auth
        return auth.register(self.ctx, name, "correct horse battery")["user"]
