"""Service/repository tests on SQLite (real migrations, real C++ engine). Run: python -m unittest discover -s tests"""
from __future__ import annotations

import sys
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.errors import ApiError
from app.models import ALGORITHMS
from app.services import auth, events, jobs, oslab, resources, runs
from tests.helpers import DbTestCase, engine_available, job, run_params


class AuthTests(DbTestCase):
    def test_first_user_is_admin_second_is_user(self):
        self.assertEqual(auth.register(self.ctx, "alice", "password-one")["user"]["role"], "admin")
        self.assertEqual(auth.register(self.ctx, "bob", "password-two")["user"]["role"], "user")

    def test_duplicate_username_conflict(self):
        auth.register(self.ctx, "alice", "password-one")
        with self.assertRaises(ApiError) as cm:
            auth.register(self.ctx, "alice", "password-two")
        self.assertEqual(cm.exception.status_code, 409)

    def test_password_is_hashed_in_db(self):
        auth.register(self.ctx, "alice", "password-one")
        with self.db.transaction() as tx:
            h = tx.scalar("SELECT password_hash FROM users WHERE username = %s", ("alice",))
        self.assertTrue(h.startswith("scrypt$"))
        self.assertNotIn("password-one", h)

    def test_login_ok_wrong_password_and_unknown_user(self):
        auth.register(self.ctx, "alice", "password-one")
        tok = auth.login(self.ctx, "alice", "password-one", "1.2.3.4")
        self.assertEqual(auth.authenticate(self.ctx, tok["access_token"])["username"], "alice")
        for u, p in (("alice", "wrong-password"), ("nobody", "whatever-pass")):
            with self.assertRaises(ApiError) as cm:
                auth.login(self.ctx, u, p, "1.2.3.4")
            self.assertEqual(cm.exception.status_code, 401)
            self.assertEqual(cm.exception.message, "Incorrect username or password")

    def test_login_is_throttled_after_repeated_failures(self):
        auth.register(self.ctx, "alice", "password-one")
        for _ in range(3):
            with self.assertRaises(ApiError):
                auth.login(self.ctx, "alice", "bad-password", "9.9.9.9")
        with self.assertRaises(ApiError) as cm:
            auth.login(self.ctx, "alice", "password-one", "9.9.9.9")   # even the right password is blocked now
        self.assertEqual(cm.exception.status_code, 429)

    def test_tampered_or_garbage_token_rejected(self):
        tok = auth.register(self.ctx, "alice", "password-one")["access_token"]
        for bad in (tok[:-3] + "abc", "garbage", ""):
            with self.assertRaises(ApiError) as cm:
                auth.authenticate(self.ctx, bad)
            self.assertEqual(cm.exception.status_code, 401)

    def test_token_of_deleted_user_stops_working(self):
        r = auth.register(self.ctx, "alice", "password-one")
        with self.db.transaction() as tx:
            tx.execute("DELETE FROM system_events")
            tx.execute("DELETE FROM users WHERE id = %s", (r["user"]["id"],))
        with self.assertRaises(ApiError):
            auth.authenticate(self.ctx, r["access_token"])


class JobTests(DbTestCase):
    def test_crud_and_status(self):
        u = self.user()
        j = jobs.create(self.ctx, u["id"], job("a"))
        self.assertEqual(j["status"], "NEW")
        j2 = jobs.update(self.ctx, u["id"], j["id"], job("a2", burst=9))
        self.assertEqual((j2["name"], j2["burst_time"]), ("a2", 9))
        jobs.delete(self.ctx, u["id"], j["id"])
        with self.assertRaises(ApiError) as cm:
            jobs.get(self.ctx, u["id"], j["id"])
        self.assertEqual(cm.exception.status_code, 404)

    def test_cancel_rules(self):
        u = self.user()
        j = jobs.create(self.ctx, u["id"], job("a"))
        self.assertEqual(jobs.cancel(self.ctx, u["id"], j["id"])["status"], "CANCELLED")
        with self.assertRaises(ApiError) as cm:
            jobs.cancel(self.ctx, u["id"], j["id"])            # already cancelled: final state
        self.assertEqual(cm.exception.status_code, 409)
        with self.assertRaises(ApiError):
            jobs.update(self.ctx, u["id"], j["id"], job("x"))   # cancelled jobs cannot be edited

    def test_users_cannot_see_or_change_each_others_jobs(self):
        a, b = self.user("alice"), self.user("bobby")
        j = jobs.create(self.ctx, a["id"], job("secret"))
        for fn in (lambda: jobs.get(self.ctx, b["id"], j["id"]),
                   lambda: jobs.update(self.ctx, b["id"], j["id"], job("x")),
                   lambda: jobs.cancel(self.ctx, b["id"], j["id"]),
                   lambda: jobs.delete(self.ctx, b["id"], j["id"])):
            with self.assertRaises(ApiError) as cm:
                fn()
            self.assertEqual(cm.exception.status_code, 404)
        self.assertEqual(jobs.list_jobs(self.ctx, b["id"], status=None, text=None, sort="id", order="asc", page=1, page_size=50)["total"], 0)

    def test_search_filter_sort_paginate_and_like_escape(self):
        u = self.user()
        for n in ("alpha", "beta", "100%_done", "gamma"):
            jobs.create(self.ctx, u["id"], job(n))
        r = jobs.list_jobs(self.ctx, u["id"], status=None, text="%", sort="name", order="asc", page=1, page_size=50)
        self.assertEqual([i["name"] for i in r["items"]], ["100%_done"])      # '%' is literal, not a wildcard
        r = jobs.list_jobs(self.ctx, u["id"], status=None, text=None, sort="name", order="desc", page=2, page_size=3)
        self.assertEqual((r["total"], [i["name"] for i in r["items"]]), (4, ["100%_done"]))
        r = jobs.list_jobs(self.ctx, u["id"], status=None, text="'; DROP TABLE jobs;--", sort="name; DROP", order="asc", page=1, page_size=5)
        self.assertEqual(r["total"], 0)                                          # injection text is just data

    def test_samples_are_flagged(self):
        u = self.user()
        items = jobs.load_samples(self.ctx, u["id"])
        self.assertTrue(items and all(i["is_sample"] for i in items))


@unittest.skipUnless(engine_available(), "build/mcrm_engine not built")
class RunTests(DbTestCase):
    def seed(self, u, specs):
        return [jobs.create(self.ctx, u["id"], job(*s[:1], **s[1])) for s in specs]

    def test_fcfs_hand_calculated_single_cpu(self):
        u = self.user()
        self.seed(u, [("A", dict(arrival=0, burst=4)), ("B", dict(arrival=1, burst=3)), ("C", dict(arrival=2, burst=2))])
        d = runs.run(self.ctx, u["id"], {"algorithm": "FCFS", **run_params(use_cluster=False)})
        m = d["run"]["metrics"]
        self.assertEqual(d["run"]["status"], "COMPLETED")
        self.assertAlmostEqual(m["avg_waiting"], (0 + 3 + 5) / 3, places=3)
        self.assertAlmostEqual(m["avg_turnaround"], (4 + 6 + 7) / 3, places=3)
        self.assertEqual((m["completed"], m["total_time"]), (3, 9))
        self.assertAlmostEqual(m["throughput"], 3 / 9, places=3)
        self.assertEqual([(s["job_name"], s["start"], s["end"]) for s in d["gantt"]], [("A", 0, 4), ("B", 4, 7), ("C", 7, 9)])

    def test_db_rows_match_engine_output_and_resources_return_to_zero(self):
        u = self.user()
        self.seed(u, [("a", dict(arrival=0, burst=4, cpu=6, ram=20, storage=300)),
                      ("b", dict(arrival=0, burst=3, cpu=6, ram=20, storage=300)),
                      ("c", dict(arrival=1, burst=2, cpu=12, ram=40, storage=600)),
                      ("huge", dict(arrival=1, burst=2, cpu=64, ram=1, storage=1))])
        d = runs.run(self.ctx, u["id"], {"algorithm": "FCFS", **run_params()})
        res = {r["job_name"]: r for r in d["results"]}
        self.assertEqual(res["huge"]["result_status"], "REJECTED")
        self.assertIn("exceeds", res["huge"]["reason"])
        for r in d["results"]:
            if r["result_status"] == "COMPLETED":
                self.assertEqual(r["turnaround_time"], r["completion_time"] - r["arrival_time"])
                self.assertEqual(r["waiting_time"], r["turnaround_time"] - r["burst_time"])
        self.assertEqual(d["run"]["metrics"]["completed"], sum(r["result_status"] == "COMPLETED" for r in d["results"]))
        srv = resources.list_servers(self.ctx)
        self.assertEqual(srv["allocated"], {"cpu": 0, "ram_gb": 0, "storage_gb": 0})   # released exactly once
        self.assertTrue(all(a["status"] == "RELEASED" and a["released_at"] >= a["allocated_at"] for a in d["allocations"]))
        # job states
        st = {j["name"]: j["status"] for j in jobs.list_jobs(self.ctx, u["id"], status=None, text=None, sort="id", order="asc", page=1, page_size=50)["items"]}
        self.assertEqual(st["huge"], "FAILED")
        self.assertTrue(all(st[n] == "COMPLETED" for n in ("a", "b", "c")))

    def test_waiting_job_is_retried_after_release(self):
        u = self.user()
        # both need 16 CPU = whole Server-2; the second must wait for the first (other servers are too small)
        self.seed(u, [("first", dict(arrival=0, burst=5, cpu=16)), ("second", dict(arrival=0, burst=5, cpu=16))])
        d = runs.run(self.ctx, u["id"], {"algorithm": "FCFS", **run_params()})
        res = {r["job_name"]: r for r in d["results"]}
        self.assertEqual((res["first"]["first_start"], res["second"]["first_start"]), (0, 5))
        ev = events.search(self.ctx, u["id"], event_type="INSUFFICIENT_RESOURCES", run_id=None, job_id=None, text=None,
                           since=None, until=None, order="asc", page=1, page_size=50)
        self.assertEqual(ev["total"], 1)
        self.assertIn("must wait", ev["items"][0]["description"])
        chg = events.search(self.ctx, u["id"], event_type="JOB_STATE_CHANGE", run_id=None, job_id=None, text="second",
                            since=None, until=None, order="asc", page=1, page_size=50)
        self.assertIn("WAITING -> RUNNING -> COMPLETED", chg["items"][0]["description"])

    def test_every_algorithm_runs_and_keeps_invariants(self):
        u = self.user()
        self.seed(u, [("p1", dict(arrival=0, burst=7, priority=3, cpu=3)), ("p2", dict(arrival=2, burst=4, priority=1, cpu=3)),
                      ("p3", dict(arrival=4, burst=1, priority=2, cpu=2)), ("p4", dict(arrival=5, burst=4, priority=1, cpu=5))])
        for alg in ALGORITHMS:
            for cluster in (True, False):
                with self.subTest(alg=alg, cluster=cluster):
                    d = runs.run(self.ctx, u["id"], {"algorithm": alg, **run_params(use_cluster=cluster, mlfq_quanta=[2, 4, 0] if alg == "MLFQ" else None)})
                    m = d["run"]["metrics"]
                    self.assertEqual(m["completed"], 4)
                    self.assertEqual(sum(s["end"] - s["start"] for s in d["gantt"] if s["type"] == "EXEC"), 7 + 4 + 1 + 4)
                    self.assertEqual(resources.list_servers(self.ctx)["allocated"]["cpu"], 0)

    def test_run_input_validation_and_empty_workload(self):
        u = self.user()
        with self.assertRaises(ApiError) as cm:
            runs.run(self.ctx, u["id"], {"algorithm": "FCFS", **run_params()})
        self.assertEqual(cm.exception.status_code, 422)
        j = jobs.create(self.ctx, u["id"], job("x"))
        with self.assertRaises(ApiError) as cm:
            runs.run(self.ctx, u["id"], {"algorithm": "FCFS", **run_params(job_ids=[j["id"], 99999])})
        self.assertEqual(cm.exception.status_code, 404)
        jobs.cancel(self.ctx, u["id"], j["id"])
        with self.assertRaises(ApiError) as cm:
            runs.run(self.ctx, u["id"], {"algorithm": "FCFS", **run_params(job_ids=[j["id"]])})
        self.assertEqual(cm.exception.status_code, 409)

    def test_cancelled_jobs_are_excluded_from_default_runs(self):
        u = self.user()
        a, b = jobs.create(self.ctx, u["id"], job("a")), jobs.create(self.ctx, u["id"], job("b"))
        jobs.cancel(self.ctx, u["id"], b["id"])
        d = runs.run(self.ctx, u["id"], {"algorithm": "FCFS", **run_params()})
        self.assertEqual([r["job_name"] for r in d["results"]], ["a"])

    def test_runs_are_isolated_between_users(self):
        a, b = self.user("alice"), self.user("bobby")
        jobs.create(self.ctx, a["id"], job("x"))
        run_id = runs.run(self.ctx, a["id"], {"algorithm": "FCFS", **run_params()})["run"]["id"]
        for fn in (runs.detail, runs.gantt, runs.results, runs.utilization_series, runs.delete_run):
            with self.assertRaises(ApiError) as cm:
                fn(self.ctx, b["id"], run_id)
            self.assertEqual(cm.exception.status_code, 404)
        self.assertEqual(runs.list_runs(self.ctx, b["id"], algorithm=None, status=None, comparison_id=None, sort="id", order="desc", page=1, page_size=20)["total"], 0)
        with self.assertRaises(ApiError):
            runs.compare_saved(self.ctx, b["id"], [run_id, run_id + 1])

    def test_comparison_saves_all_runs_and_best_by_metric(self):
        u = self.user()
        # all arrive at 0: FCFS order long, short, mid -> waits 0, 8, 9 (avg 17/3); SJF order short, mid, long -> 0, 1, 4 (avg 5/3)
        self.seed(u, [("long", dict(arrival=0, burst=8)), ("short", dict(arrival=0, burst=1)), ("mid", dict(arrival=0, burst=3))])
        c = runs.compare(self.ctx, u["id"], {"name": "FCFS vs SJF", "algorithms": ["FCFS", "SJF"], **run_params(use_cluster=False)})
        self.assertEqual([r["algorithm"] for r in c["runs"]], ["FCFS", "SJF"])
        best = {b["metric"]: b["best"] for b in c["comparison"]["best_by_metric"]}
        self.assertEqual(best["avg_waiting"], ["SJF"])
        by_alg = {r["algorithm"]: r["metrics"]["avg_waiting"] for r in c["runs"]}
        self.assertAlmostEqual(by_alg["FCFS"], 17 / 3, places=3)
        self.assertAlmostEqual(by_alg["SJF"], 5 / 3, places=3)
        self.assertEqual(runs.list_comparisons(self.ctx, u["id"], 1, 10)["total"], 1)
        ids = [r["id"] for r in c["runs"]]
        self.assertEqual(len(runs.compare_saved(self.ctx, u["id"], ids)["best_by_metric"]), 6)

    def test_history_filter_sort_pagination_and_delete_cascades(self):
        u = self.user()
        jobs.create(self.ctx, u["id"], job("x"))
        ids = [runs.run(self.ctx, u["id"], {"algorithm": a, **run_params()})["run"]["id"] for a in ("FCFS", "SJF", "FCFS")]
        r = runs.list_runs(self.ctx, u["id"], algorithm="FCFS", status=None, comparison_id=None, sort="id", order="asc", page=1, page_size=1)
        self.assertEqual((r["total"], len(r["items"]), r["items"][0]["id"]), (2, 1, ids[0]))
        runs.delete_run(self.ctx, u["id"], ids[0])
        with self.db.transaction() as tx:
            for t in ("scheduling_results", "execution_segments", "resource_allocations"):
                self.assertEqual(tx.scalar(f"SELECT COUNT(*) FROM {t} WHERE run_id = %s", (ids[0],)), 0)
            self.assertGreater(tx.scalar("SELECT COUNT(*) FROM scheduling_results WHERE run_id = %s", (ids[1],)), 0)

    def test_deleting_a_job_keeps_run_history(self):
        u = self.user()
        j = jobs.create(self.ctx, u["id"], job("gone"))
        run_id = runs.run(self.ctx, u["id"], {"algorithm": "FCFS", **run_params()})["run"]["id"]
        jobs.delete(self.ctx, u["id"], j["id"])
        d = runs.detail(self.ctx, u["id"], run_id)
        self.assertEqual((d["results"][0]["job_name"], d["results"][0]["job_id"]), ("gone", None))

    def test_utilization_series_and_allocation_history(self):
        u = self.user()
        jobs.create(self.ctx, u["id"], job("a", burst=4, cpu=2))
        run_id = runs.run(self.ctx, u["id"], {"algorithm": "FCFS", **run_params()})["run"]["id"]
        s = runs.utilization_series(self.ctx, u["id"], run_id)["series"]
        self.assertEqual([p["time"] for p in s], [0, 4])
        self.assertGreater(max(sv["cpu_percent"] for sv in s[0]["servers"]), 0)
        self.assertTrue(all(sv["cpu_percent"] == 0 for sv in s[1]["servers"]))
        h = resources.allocation_history(self.ctx, u["id"], run_id=run_id, job_id=None, status="RELEASED", server=None, order="asc", page=1, page_size=10)
        self.assertEqual(h["total"], 1)

    def test_concurrent_runs_never_exceed_capacity(self):
        u = self.user()
        jobs.load_samples(self.ctx, u["id"])
        errors, out = [], []

        def worker(alg):
            try:
                out.append(runs.run(self.ctx, u["id"], {"algorithm": alg, **run_params()})["run"]["id"])
            except Exception as exc:       # noqa: BLE001
                errors.append(exc)
        ts = [threading.Thread(target=worker, args=(a,)) for a in ("FCFS", "SJF", "RR", "SRTF", "PRIORITY_NP", "HRRN")]
        [t.start() for t in ts]; [t.join() for t in ts]
        self.assertEqual(errors, [])
        self.assertEqual(len(set(out)), 6)
        self.assertEqual(resources.list_servers(self.ctx)["allocated"], {"cpu": 0, "ram_gb": 0, "storage_gb": 0})


class SchemaConstraintTests(DbTestCase):
    def test_database_itself_refuses_over_allocation_and_negative(self):
        from app.repositories import resources as rr
        with self.assertRaises(self.db.integrity_errors):
            with self.db.transaction() as tx:
                rr.apply_delta(tx, 3, 5, 0, 0)          # Server-3 has 4 cores
        with self.assertRaises(self.db.integrity_errors):
            with self.db.transaction() as tx:
                rr.apply_delta(tx, 1, -1, 0, 0)
        self.assertEqual(resources.list_servers(self.ctx)["allocated"]["cpu"], 0)   # rolled back

    def test_invalid_rows_are_rejected(self):
        u = self.user()
        bad = [dict(job(), burst_time=0), dict(job(), cpu_cores=0), dict(job(), arrival_time=-1)]
        from app.repositories import jobs as jr
        for b in bad:
            with self.assertRaises(self.db.integrity_errors):
                with self.db.transaction() as tx:
                    jr.insert(tx, u["id"], b)
        with self.assertRaises(self.db.integrity_errors):
            with self.db.transaction() as tx:
                tx.execute("UPDATE jobs SET status = 'BOGUS' WHERE 1=0 OR id = (SELECT MIN(id) FROM jobs)") if False else tx.execute(
                    "INSERT INTO jobs (user_id, name, burst_time, status) VALUES (%s, 'x', 1, 'BOGUS')", (u["id"],))


class ResourceAdminTests(DbTestCase):
    def test_only_admin_may_change_capacity_and_not_below_allocated(self):
        admin, user = self.user("alice"), self.user("bobby")
        data = {"name": "Server-3", "total_cpu": 6, "total_ram_gb": 16, "total_storage_gb": 250}
        with self.assertRaises(ApiError) as cm:
            resources.update_server(self.ctx, user, 3, data)
        self.assertEqual(cm.exception.status_code, 403)
        self.assertEqual(resources.update_server(self.ctx, admin, 3, data)["total"]["cpu"], 6)
        with self.assertRaises(ApiError) as cm:
            resources.update_server(self.ctx, admin, 99, data)
        self.assertEqual(cm.exception.status_code, 404)
        with self.assertRaises(ApiError) as cm:                       # duplicate name -> 422, not 500
            resources.update_server(self.ctx, admin, 3, dict(data, name="Server-1"))
        self.assertEqual(cm.exception.status_code, 422)


@unittest.skipUnless(engine_available(), "build/mcrm_engine not built")
class OsLabTests(DbTestCase):
    BANKER = {"available": [3, 3, 2], "max": [[7, 5, 3], [3, 2, 2], [9, 0, 2], [2, 2, 2], [4, 3, 3]],
              "allocation": [[0, 1, 0], [2, 0, 0], [3, 0, 2], [2, 1, 1], [0, 0, 2]]}

    def test_banker_checks_are_persisted(self):
        u = self.user()
        o = oslab.banker(self.ctx, u["id"], {"action": "safety", **self.BANKER})
        self.assertTrue(o["safety"]["safe"])
        o = oslab.banker(self.ctx, u["id"], {"action": "request", "process": 1, "request": [1, 0, 2], **self.BANKER})
        self.assertTrue(o["granted"])
        o = oslab.banker(self.ctx, u["id"], {"action": "request", "process": 4, "request": [3, 3, 0], **self.BANKER})
        self.assertFalse(o["granted"])
        o = oslab.banker(self.ctx, u["id"], {"action": "detect", "available": [0, 0], "allocation": [[1, 0], [0, 1]], "request": [[0, 1], [1, 0]]})
        self.assertTrue(o["deadlocked"])
        h = oslab.history(self.ctx, u["id"], outcome=None, order="asc", page=1, page_size=10)
        self.assertEqual([i["outcome"] for i in h["items"]], ["SAFE", "GRANTED", "DENIED", "DEADLOCK"])
        self.assertEqual(h["items"][0]["safe_sequence"], "P1,P3,P4,P0,P2")
        self.assertEqual(oslab.history(self.ctx, u["id"], outcome="DEADLOCK", order="asc", page=1, page_size=10)["total"], 1)
        other = self.user("bobby")
        self.assertEqual(oslab.history(self.ctx, other["id"], outcome=None, order="asc", page=1, page_size=10)["total"], 0)

    def test_engine_validation_error_is_422(self):
        u = self.user()
        with self.assertRaises(ApiError) as cm:
            oslab.banker(self.ctx, u["id"], {"action": "safety", "available": [1, 1], "max": [[1, 1, 1]], "allocation": [[0, 0]]})
        self.assertEqual(cm.exception.status_code, 422)

    def test_memory_and_sync_modules(self):
        u = self.user()
        m = oslab.memory(self.ctx, u["id"], {"strategy": "BEST_FIT", "partitions": [100, 500, 200, 300, 600],
                                             "operations": [{"type": "ALLOC", "process": "A", "size": 212}]})
        self.assertTrue(m["steps"][0]["success"])
        s = oslab.sync(self.ctx, u["id"], {"scenario": "race", "increments": 5, "use_mutex": True})
        self.assertTrue(s["ok"])


if __name__ == "__main__":
    unittest.main()
