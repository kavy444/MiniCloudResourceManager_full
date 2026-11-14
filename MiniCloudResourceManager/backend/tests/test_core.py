"""Unit tests that need no database and no engine: security, config, validation models, state machine, migrations."""
from __future__ import annotations

import ast
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pydantic import ValidationError

from app import migrate, models, security, states
from app.config import ConfigError, Settings
from app.errors import ApiError
from app.repositories import like_pattern, page_args

BACKEND = Path(__file__).resolve().parents[1]
PROJECT = BACKEND.parent


class SecurityTests(unittest.TestCase):
    def test_password_hash_roundtrip_and_salting(self):
        h1, h2 = security.hash_password("s3cret-pass"), security.hash_password("s3cret-pass")
        self.assertNotEqual(h1, h2)                                # random salt
        self.assertTrue(security.verify_password("s3cret-pass", h1))
        self.assertFalse(security.verify_password("s3cret-pasS", h1))
        self.assertFalse(security.verify_password("x", "not-a-hash"))
        self.assertTrue(security.DUMMY_HASH.startswith("scrypt$"))

    def test_tokens(self):
        secret = "k" * 40
        token, ttl = security.create_token(7, secret, 10)
        self.assertEqual((security.decode_token(token, secret), ttl), (7, 600))
        for bad_secret, tok in (("z" * 40, token), (secret, token + "x")):
            with self.assertRaises(ApiError) as cm:
                security.decode_token(tok, bad_secret)
            self.assertEqual(cm.exception.status_code, 401)
        expired, _ = security.create_token(7, secret, -1)
        with self.assertRaises(ApiError) as cm:
            security.decode_token(expired, secret)
        self.assertIn("expired", cm.exception.message)

    def test_login_limiter_window(self):
        now = [0.0]
        lim = security.LoginLimiter(max_failures=2, window_s=10, clock=lambda: now[0])
        lim.record_failure("k"); lim.record_failure("k")
        with self.assertRaises(ApiError):
            lim.check("k")
        lim.check("other")                                        # keys are independent
        now[0] = 11.0
        lim.check("k")                                            # window passed
        lim.record_failure("k"); lim.reset("k"); lim.record_failure("k"); lim.check("k")


class ConfigTests(unittest.TestCase):
    BASE = {"JWT_SECRET": "x" * 40, "DB_PASSWORD": "p"}

    def test_required_secrets(self):
        for env in ({"DB_PASSWORD": "p"}, {"JWT_SECRET": "x" * 40}, {"JWT_SECRET": "short", "DB_PASSWORD": "p"}):
            with self.assertRaises(ConfigError):
                Settings.from_env(env)

    def test_cors_and_numbers(self):
        with self.assertRaises(ConfigError):
            Settings.from_env({**self.BASE, "CORS_ORIGINS": "http://a.test, *"})
        with self.assertRaises(ConfigError):
            Settings.from_env({**self.BASE, "DB_PORT": "abc"})
        s = Settings.from_env({**self.BASE, "CORS_ORIGINS": "http://a.test, http://b.test"})
        self.assertEqual(s.cors_origins, ("http://a.test", "http://b.test"))
        self.assertNotIn("p", repr(s).replace("pool", "").replace("path", "").split("db_password")[-1][:6])   # repr hides it

    def test_password_not_in_repr(self):
        s = Settings.from_env({"JWT_SECRET": "UNIQUE-JWT-" + "x" * 30, "DB_PASSWORD": "UNIQUE-DB-PASSWORD"})
        self.assertNotIn("UNIQUE-DB-PASSWORD", repr(s))
        self.assertNotIn("UNIQUE-JWT", repr(s))

    def test_repository_hygiene(self):
        ignore = (PROJECT / ".gitignore").read_text().splitlines()
        self.assertIn(".env", ignore)
        example = (BACKEND / ".env.example").read_text()
        self.assertIn("DB_PASSWORD=change-me", example)
        # no hard-coded credentials: a quoted literal assigned to a password/secret/token name anywhere in the shipped code
        import re
        literal = re.compile(r"""(password|passwd|secret|api_key|token)\w*\s*[=:]\s*["'][^"'\s]{6,}["']""", re.IGNORECASE)
        shipped = list((BACKEND / "app").rglob("*.py")) + list((BACKEND / "scripts").rglob("*.py")) + \
            [p for p in (PROJECT / "frontend").rglob("*") if p.is_file()] + list((BACKEND / "migrations").glob("*.sql"))
        allowed = ('TOKEN_KEY = "mcrm_token"',)      # name of the sessionStorage slot, not a secret value
        for path in shipped:
            for line in path.read_text(errors="ignore").splitlines():
                if any(a in line for a in allowed):
                    continue
                self.assertIsNone(literal.search(line), f"{path}: {line.strip()}")


class ModelTests(unittest.TestCase):
    def test_job_limits(self):
        ok = dict(name="a", burst_time=1)
        models.JobIn(**ok)
        for bad in (dict(ok, burst_time=0), dict(ok, burst_time=10_001), dict(ok, name=""), dict(ok, name="\x00x"),
                    dict(ok, cpu_cores=0), dict(ok, arrival_time=-1), dict(ok, priority=0), dict(ok, queue_level=8),
                    dict(ok, extra=1), dict(ok, burst_time="5")):
            with self.assertRaises(ValidationError, msg=str(bad)):
                models.JobIn(**bad)

    def test_credentials_normalise_and_validate(self):
        self.assertEqual(models.Credentials(username="  Bob.Smith ", password="12345678").username, "bob.smith")
        for bad in ({"username": "a b", "password": "12345678"}, {"username": "bob", "password": "1234567"},
                    {"username": "bob", "password": "x" * 129}):
            with self.assertRaises(ValidationError):
                models.Credentials(**bad)

    def test_scheduler_params(self):
        models.RunRequest(algorithm="RR", time_quantum=3)
        for bad in (dict(algorithm="NOPE"), dict(algorithm="RR", time_quantum=0),
                    dict(algorithm="MLQ", mlq_queues=[{"policy": "RR", "quantum": 0}]),
                    dict(algorithm="MLFQ", mlfq_quanta=[-1]), dict(algorithm="FCFS", job_ids=[1, 1]),
                    dict(algorithm="FCFS", job_ids=[0]), dict(algorithm="MLFQ", mlfq_quanta=[])):
            with self.assertRaises(ValidationError, msg=str(bad)):
                models.RunRequest(**bad)
        with self.assertRaises(ValidationError):
            models.ComparisonRequest(algorithms=["FCFS"])
        with self.assertRaises(ValidationError):
            models.ComparisonRequest(algorithms=["FCFS", "FCFS"])

    def test_os_models(self):
        with self.assertRaises(ValidationError):
            models.MemoryRequest(strategy="FIRST_FIT", operations=[{"type": "ALLOC", "process": "A", "size": 1}])
        with self.assertRaises(ValidationError):
            models.MemoryRequest(strategy="FIRST_FIT", total_size=10, partitions=[5],
                                 operations=[{"type": "ALLOC", "process": "A", "size": 1}])
        with self.assertRaises(ValidationError):
            models.BankerRequest(action="request", available=[1], allocation=[[0]], max=[[1]])
        with self.assertRaises(ValidationError):
            models.BankerRequest(action="safety", available=[1], allocation=[[0]])


class StateTests(unittest.TestCase):
    def test_documented_transitions(self):
        legal = {("NEW", "READY"), ("READY", "RUNNING"), ("READY", "WAITING"), ("WAITING", "RUNNING"),
                 ("RUNNING", "COMPLETED"), ("READY", "FAILED"), ("WAITING", "FAILED"), ("NEW", "CANCELLED"),
                 ("READY", "CANCELLED"), ("WAITING", "CANCELLED"), ("COMPLETED", "READY"), ("FAILED", "NEW")}
        for a, b in legal:
            self.assertTrue(states.can_transition(a, b), (a, b))
        for a, b in (("COMPLETED", "RUNNING"), ("RUNNING", "WAITING"), ("NEW", "RUNNING"), ("NEW", "COMPLETED"),
                     ("RUNNING", "CANCELLED"), ("WAITING", "COMPLETED")):
            self.assertFalse(states.can_transition(a, b), (a, b))
        for target in states.ALL_STATES:
            self.assertFalse(states.can_transition("CANCELLED", target))     # final state
        with self.assertRaises(ValueError):
            states.assert_transition("COMPLETED", "RUNNING")

    def test_run_paths(self):
        self.assertEqual(states.run_path("NEW", "COMPLETED", False), ["READY", "RUNNING", "COMPLETED"])
        self.assertEqual(states.run_path("NEW", "COMPLETED", True), ["READY", "WAITING", "RUNNING", "COMPLETED"])
        self.assertEqual(states.run_path("NEW", "REJECTED", False), ["READY", "FAILED"])
        self.assertEqual(states.run_path("COMPLETED", "COMPLETED", False), ["READY", "RUNNING", "COMPLETED"])
        self.assertEqual(states.run_path("FAILED", "REJECTED", False), ["READY", "FAILED"])

    def test_cancellable_set(self):
        self.assertEqual(states.CANCELLABLE, {"NEW", "READY", "WAITING"})


class MigrationTests(unittest.TestCase):
    def test_split_statements(self):
        sql = "-- comment\nCREATE TABLE a (x INT);\n\n-- another\nINSERT INTO a VALUES (1);\n"
        self.assertEqual(migrate.split_statements(sql), ["CREATE TABLE a (x INT)", "INSERT INTO a VALUES (1)"])

    def test_migrations_apply_once_and_create_required_tables(self):
        from tests.sqlite_backend import make_database, translate_ddl
        db, path = make_database()
        try:
            self.assertEqual(migrate.apply_all(db, translate=translate_ddl, log=lambda *_: None), [])   # idempotent
            with db.transaction() as tx:
                names = {r["name"] for r in tx.fetch_all("SELECT name FROM sqlite_master WHERE type = 'table'")}
                versions = [r["version"] for r in tx.fetch_all("SELECT version FROM schema_migrations ORDER BY version")]
            for table in ("users", "jobs", "scheduling_runs", "scheduling_results", "execution_segments", "resources",
                          "resource_allocations", "system_events", "deadlock_checks", "algorithm_comparisons"):
                self.assertIn(table, names)
            self.assertEqual(versions, ["001_schema.sql", "002_seed_resources.sql"])
        finally:
            db.close()

    def test_every_foreign_key_has_a_delete_rule_and_every_check_is_named(self):
        sql = (BACKEND / "migrations" / "001_schema.sql").read_text()
        fk = sql.count("FOREIGN KEY")
        self.assertGreater(fk, 8)
        self.assertEqual(fk, sql.count("ON DELETE"))
        self.assertEqual(sql.count("CHECK ("), sql.count("CONSTRAINT ck_"))


class HelperTests(unittest.TestCase):
    def test_like_pattern_escapes_wildcards(self):
        self.assertEqual(like_pattern("50%_x!"), "%50!%!_x!!%")

    def test_page_args_caps(self):
        self.assertEqual(page_args(1, 20), (20, 0))
        self.assertEqual(page_args(3, 10), (10, 20))
        self.assertEqual(page_args(0, 10_000), (200, 0))
        self.assertEqual(page_args(-5, 0), (1, 0))


class RouterConsistencyTests(unittest.TestCase):
    """Static check (works without FastAPI): every `xxx_service.func(...)` / `module.func(...)` call in the routers refers
    to a function that exists, and every service function the routers call accepts the keyword arguments passed."""

    def test_router_calls_resolve(self):
        import importlib
        import inspect
        aliases = {"auth_service": "auth", "jobs_service": "jobs", "runs_service": "runs", "resources_service": "resources",
                   "events_service": "events", "dashboard": "dashboard", "oslab": "oslab"}
        checked = 0
        for path in (BACKEND / "app" / "routers").glob("*.py"):
            tree = ast.parse(path.read_text())
            for node in ast.walk(tree):
                if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                        and isinstance(node.func.value, ast.Name) and node.func.value.id in aliases):
                    module = importlib.import_module(f"app.services.{aliases[node.func.value.id]}")
                    fn = getattr(module, node.func.attr, None)
                    self.assertTrue(callable(fn), f"{path.name}: {node.func.value.id}.{node.func.attr} does not exist")
                    sig = inspect.signature(fn)
                    sig.bind(*[None] * len(node.args), **{k.arg: None for k in node.keywords if k.arg})
                    checked += 1
        self.assertGreater(checked, 25)


if __name__ == "__main__":
    unittest.main()
