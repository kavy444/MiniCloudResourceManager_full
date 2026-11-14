"""HTTP-level tests: a real uvicorn server on 127.0.0.1 with the FastAPI app, a temporary SQLite database and the real
C++ engine. Uses only the standard library as the HTTP client. Skipped automatically when fastapi/uvicorn are missing."""
from __future__ import annotations

import json
import os
import socket
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

try:
    import fastapi  # noqa: F401
    import uvicorn
    HAVE_WEB = True
except ImportError:          # pragma: no cover
    HAVE_WEB = False

from tests.helpers import engine_available, make_context


def call(base, method, path, body=None, token=None, headers=None, raw=None):
    data = raw if raw is not None else (None if body is None else json.dumps(body).encode())
    req = urllib.request.Request(base + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            text = resp.read().decode()
            try:
                return resp.status, (json.loads(text) if text else None), resp.headers
            except ValueError:                      # e.g. the HTML frontend
                return resp.status, text, resp.headers
    except urllib.error.HTTPError as err:
        text = err.read().decode()
        try:
            return err.code, json.loads(text), err.headers
        except ValueError:
            return err.code, text, err.headers


@unittest.skipUnless(HAVE_WEB, "fastapi/uvicorn not installed")
@unittest.skipUnless(engine_available(), "build/mcrm_engine not built")
class ApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from app.main import create_app
        from tests.sqlite_backend import make_database
        cls.db, cls.path = make_database(pool_size=6)
        cls.ctx = make_context(cls.db)
        s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
        cls.base = f"http://127.0.0.1:{port}"
        cls.server = uvicorn.Server(uvicorn.Config(create_app(cls.ctx), host="127.0.0.1", port=port, log_level="critical"))
        cls.thread = threading.Thread(target=cls.server.run, daemon=True)
        cls.thread.start()
        for _ in range(100):
            if cls.server.started:
                break
            time.sleep(0.05)
        cls.admin = cls.register("admin_user")          # first account = administrator
        cls.token = cls.admin["access_token"]

    @classmethod
    def tearDownClass(cls):
        cls.server.should_exit = True
        cls.thread.join(10)
        cls.db.close()
        os.unlink(cls.path)

    @classmethod
    def register(cls, name, password="a-good-password"):
        status, body, _ = call(cls.base, "POST", "/api/auth/register", {"username": name, "password": password})
        assert status == 201, body
        return body

    def api(self, method, path, body=None, token=None, **kw):
        return call(self.base, method, path, body, token or self.token, **kw)

    JOB = {"name": "web", "arrival_time": 0, "burst_time": 4, "priority": 2, "cpu_cores": 2, "ram_gb": 4, "storage_gb": 10}

    # ---- basics
    def test_health_and_security_headers(self):
        status, body, headers = call(self.base, "GET", "/api/health")
        self.assertEqual((status, body["status"], body["database"], body["engine"]), (200, "ok", "up", "found"))
        self.assertEqual(headers["X-Content-Type-Options"], "nosniff")
        self.assertEqual(headers["Cache-Control"], "no-store")

    def test_frontend_is_served_from_same_origin(self):
        status, body, _ = call(self.base, "GET", "/")
        self.assertEqual(status, 200)
        self.assertIn("<html", body.lower())

    def test_unauthenticated_requests_are_rejected_with_json_error(self):
        for method, path in (("GET", "/api/jobs"), ("POST", "/api/runs"), ("GET", "/api/resources"),
                             ("GET", "/api/events"), ("GET", "/api/runs/1")):
            status, body, _ = call(self.base, method, path)
            self.assertEqual(status, 401, path)
            self.assertEqual(body["error"]["code"], "unauthorized")
        status, _, _ = call(self.base, "GET", "/api/jobs", token="not.a.token")
        self.assertEqual(status, 401)

    def test_malformed_json_and_unknown_fields(self):
        status, body, _ = self.api("POST", "/api/jobs", raw=b"{not json")
        self.assertEqual((status, body["error"]["code"]), (422, "validation_error"))
        status, body, _ = self.api("POST", "/api/jobs", dict(self.JOB, user_id=1))   # clients cannot send a user id
        self.assertEqual(status, 422)

    # ---- auth
    def test_register_validation_and_duplicates(self):
        for payload in ({"username": "ab", "password": "a-good-password"}, {"username": "ok_name", "password": "short"},
                        {"username": "bad name!", "password": "a-good-password"}, {"username": "x" * 60, "password": "a-good-password"}):
            self.assertEqual(call(self.base, "POST", "/api/auth/register", payload)[0], 422, payload)
        self.register("dup_user")
        status, body, _ = call(self.base, "POST", "/api/auth/register", {"username": "DUP_USER", "password": "a-good-password"})
        self.assertEqual((status, body["error"]["code"]), (409, "conflict"))

    def test_login_me_and_no_hash_leak(self):
        self.register("login_user")
        status, body, _ = call(self.base, "POST", "/api/auth/login", {"username": "login_user", "password": "a-good-password"})
        self.assertEqual(status, 200)
        self.assertNotIn("password", json.dumps(body["user"]).lower())
        status, me, _ = call(self.base, "GET", "/api/auth/me", token=body["access_token"])
        self.assertEqual(me["user"]["username"], "login_user")

    def test_login_throttle(self):
        self.register("throttle_user")
        codes = [call(self.base, "POST", "/api/auth/login", {"username": "throttle_user", "password": "wrong-password"})[0] for _ in range(4)]
        self.assertEqual(codes, [401, 401, 401, 429])

    # ---- jobs
    def test_job_validation_errors(self):
        bad = [dict(self.JOB, burst_time=0), dict(self.JOB, cpu_cores=-1), dict(self.JOB, ram_gb=0), dict(self.JOB, name=""),
               dict(self.JOB, priority=0), dict(self.JOB, arrival_time=-3), dict(self.JOB, burst_time="abc"),
               dict(self.JOB, name="x" * 101), dict(self.JOB, burst_time=10 ** 9)]
        for payload in bad:
            status, body, _ = self.api("POST", "/api/jobs", payload)
            self.assertEqual(status, 422, payload)
            self.assertTrue(body["error"]["details"])

    def test_job_crud_and_isolation(self):
        other = self.register("other_job_user")["access_token"]
        status, job, _ = self.api("POST", "/api/jobs", self.JOB)
        self.assertEqual((status, job["status"]), (201, "NEW"))
        jid = job["id"]
        self.assertEqual(self.api("GET", f"/api/jobs/{jid}")[0], 200)
        self.assertEqual(call(self.base, "GET", f"/api/jobs/{jid}", token=other)[0], 404)
        self.assertEqual(call(self.base, "PUT", f"/api/jobs/{jid}", dict(self.JOB, name="hacked"), token=other)[0], 404)
        self.assertEqual(call(self.base, "DELETE", f"/api/jobs/{jid}", token=other)[0], 404)
        self.assertEqual(self.api("PUT", f"/api/jobs/{jid}", dict(self.JOB, name="renamed"))[1]["name"], "renamed")
        self.assertEqual(self.api("POST", f"/api/jobs/{jid}/cancel")[1]["status"], "CANCELLED")
        self.assertEqual(self.api("POST", f"/api/jobs/{jid}/cancel")[0], 409)
        self.assertEqual(self.api("DELETE", f"/api/jobs/{jid}")[0], 204)
        self.assertEqual(self.api("GET", f"/api/jobs/{jid}")[0], 404)
        self.assertEqual(self.api("GET", "/api/jobs/abc")[0], 422)

    def test_job_listing_query_validation(self):
        for q in ("page=0", "page_size=500", "status=BOGUS", "order=sideways", "sort=id;drop"):
            self.assertEqual(self.api("GET", "/api/jobs?" + q)[0], 422, q)
        self.assertEqual(self.api("GET", "/api/jobs?sort=not_a_column")[0], 200)     # unknown column -> safe default

    # ---- runs
    def test_run_flow_end_to_end(self):
        token = self.register("run_user")["access_token"]
        c = lambda m, p, b=None: call(self.base, m, p, b, token=token)
        self.assertEqual(c("POST", "/api/runs", {"algorithm": "FCFS"})[0], 422)                      # no jobs yet
        self.assertEqual(c("POST", "/api/jobs/samples")[0], 201)
        self.assertEqual(c("POST", "/api/runs", {"algorithm": "BOGUS"})[0], 422)
        self.assertEqual(c("POST", "/api/runs", {"algorithm": "RR", "time_quantum": 0})[0], 422)
        status, run, _ = c("POST", "/api/runs", {"algorithm": "RR", "time_quantum": 3})
        self.assertEqual(status, 201)
        rid = run["run"]["id"]
        self.assertEqual(run["run"]["metrics"]["completed"], len(run["results"]))
        self.assertTrue(run["gantt"])
        self.assertEqual(c("GET", f"/api/runs/{rid}/gantt")[1]["algorithm"], "RR")
        self.assertEqual(len(c("GET", f"/api/runs/{rid}/results")[1]["results"]), len(run["results"]))
        self.assertTrue(c("GET", f"/api/runs/{rid}/utilization")[1]["series"])
        status, hist, _ = c("GET", "/api/runs?algorithm=RR&page_size=1&sort=avg_waiting&order=asc")
        self.assertEqual((status, hist["total"], len(hist["items"])), (200, 1, 1))
        status, cmp_, _ = c("POST", "/api/comparisons", {"name": "t", "algorithms": ["FCFS", "SJF", "SRTF"], "use_cluster": False})
        self.assertEqual(status, 201)
        self.assertEqual(len(cmp_["runs"]), 3)
        self.assertEqual(c("GET", f"/api/comparisons/{cmp_['comparison']['id']}")[0], 200)
        status, saved, _ = c("POST", "/api/runs/compare-saved", {"run_ids": [rid, cmp_["runs"][0]["id"]]})
        self.assertEqual((status, len(saved["best_by_metric"])), (200, 6))
        self.assertEqual(c("POST", "/api/runs/compare-saved", {"run_ids": [rid]})[0], 422)
        status, alloc, _ = c("GET", f"/api/allocations?run_id={rid}&status=RELEASED")
        self.assertGreater(alloc["total"], 0)
        status, ev, _ = c("GET", f"/api/events?run_id={rid}&event_type=JOB_STATE_CHANGE")
        self.assertGreater(ev["total"], 0)
        # another user cannot reach it
        stranger = self.register("stranger_user")["access_token"]
        for path in (f"/api/runs/{rid}", f"/api/runs/{rid}/gantt", f"/api/runs/{rid}/utilization"):
            self.assertEqual(call(self.base, "GET", path, token=stranger)[0], 404)
        self.assertEqual(call(self.base, "DELETE", f"/api/runs/{rid}", token=stranger)[0], 404)
        self.assertEqual(c("DELETE", f"/api/runs/{rid}")[0], 204)

    def test_event_filters_validate_dates(self):
        self.assertEqual(self.api("GET", "/api/events?since=yesterday")[0], 422)
        self.assertEqual(self.api("GET", "/api/events?since=2020-01-01&until=2099-01-01&q=%25")[0], 200)
        self.assertEqual(self.api("GET", "/api/events?event_type=lower")[0], 422)

    # ---- resources
    def test_resources_and_admin_only_edit(self):
        status, body, _ = self.api("GET", "/api/resources")
        self.assertEqual((status, len(body["servers"])), (200, 3))
        user = self.register("plain_user")["access_token"]
        data = {"name": "Server-3", "total_cpu": 4, "total_ram_gb": 16, "total_storage_gb": 250}
        self.assertEqual(call(self.base, "PUT", "/api/resources/3", data, token=user)[0], 403)
        self.assertEqual(self.api("PUT", "/api/resources/3", data)[0], 200)
        self.assertEqual(self.api("PUT", "/api/resources/3", dict(data, total_cpu=0))[0], 422)

    # ---- OS modules
    def test_os_modules(self):
        b = {"action": "safety", "available": [3, 3, 2], "max": [[7, 5, 3], [3, 2, 2], [9, 0, 2], [2, 2, 2], [4, 3, 3]],
             "allocation": [[0, 1, 0], [2, 0, 0], [3, 0, 2], [2, 1, 1], [0, 0, 2]]}
        status, out, _ = self.api("POST", "/api/os/banker", b)
        self.assertEqual((status, out["safety"]["safe"]), (200, True))
        self.assertEqual(self.api("POST", "/api/os/banker", {"action": "request", "available": [1], "allocation": [[0]]})[0], 422)
        self.assertGreater(self.api("GET", "/api/os/deadlock-checks?outcome=SAFE")[1]["total"], 0)
        self.assertEqual(self.api("POST", "/api/os/memory", {"strategy": "FIRST_FIT", "total_size": 100,
                         "operations": [{"type": "ALLOC", "process": "A", "size": 30}]})[0], 200)
        self.assertEqual(self.api("POST", "/api/os/sync", {"scenario": "race", "use_mutex": False})[0], 200)

    # ---- cross-cutting
    def test_cors_only_for_configured_origin(self):
        _, _, h = call(self.base, "GET", "/api/health", headers={"Origin": "http://localhost:8000"})
        self.assertEqual(h.get("Access-Control-Allow-Origin"), "http://localhost:8000")
        _, _, h = call(self.base, "GET", "/api/health", headers={"Origin": "http://evil.example"})
        self.assertIsNone(h.get("Access-Control-Allow-Origin"))

    def test_internal_errors_do_not_leak_details(self):
        from app.services import dashboard
        import logging
        logging.getLogger("mcrm").disabled = True          # the server logs the traceback; keep test output clean
        try:
            with mock.patch.object(dashboard, "overview", side_effect=RuntimeError("secret-db-detail")):
                status, body, _ = self.api("GET", "/api/dashboard")
        finally:
            logging.getLogger("mcrm").disabled = False
        self.assertEqual((status, body["error"]["code"]), (500, "internal_error"))
        self.assertNotIn("secret", json.dumps(body))

    def test_dashboard_overview(self):
        status, body, _ = self.api("GET", "/api/dashboard")
        self.assertEqual(status, 200)
        self.assertIn("by_status", body["jobs"])
        self.assertEqual(len(body["cluster"]["servers"]), 3)

    def test_algorithm_catalog(self):
        self.assertEqual(len(self.api("GET", "/api/meta/algorithms")[1]["items"]), 9)


if __name__ == "__main__":
    unittest.main()
