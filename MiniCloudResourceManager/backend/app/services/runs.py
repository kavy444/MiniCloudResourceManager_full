"""Scheduling runs: DB jobs -> C++ engine -> rows in MySQL.

A run is a self-contained simulation on an idle cluster. All scheduling decisions (order, preemption, placement,
waiting) are made by the C++ engine. Python only prepares the input, validates the output, and stores it in ONE
transaction together with the replayed resource usage and the job state changes.
"""
from __future__ import annotations

from .. import states
from ..errors import conflict, not_found, ApiError, unprocessable
from ..repositories import events as events_repo
from ..repositories import jobs as jobs_repo
from ..repositories import resources as resources_repo
from ..repositories import runs as runs_repo

MAX_SEGMENTS = 20_000       # keeps a single run (and its Gantt chart) a sensible size
MAX_STORED_EVENTS = 3_000   # engine events + decision-log lines stored per run

METRIC_KEYS = ("total_processes", "completed", "rejected", "avg_waiting", "avg_turnaround", "avg_response",
               "throughput", "cpu_utilization", "context_switches", "preemptions", "total_time", "start_time")


# ------------------------------------------------------------------ building the engine request
def engine_config(params: dict, servers: list[dict]) -> dict:
    config: dict = {"time_quantum": params["time_quantum"]}
    if params.get("mlq_queues"):
        config["mlq"] = [{"policy": q["policy"], "quantum": q["quantum"]} for q in params["mlq_queues"]]
    if params.get("mlfq_quanta"):
        config["mlfq"] = {"quanta": list(params["mlfq_quanta"]), "aging": params.get("mlfq_aging", 0)}
    elif params.get("mlfq_aging"):
        config["mlfq"] = {"aging": params["mlfq_aging"]}
    if params.get("use_cluster", True):
        config["cluster"] = {"enabled": True, "servers": [
            {"id": s["id"], "name": s["name"], "cpu": s["total_cpu"], "ram_gb": s["total_ram_gb"],
             "storage_gb": s["total_storage_gb"]} for s in servers]}
    return config


def engine_processes(jobs: list[dict]) -> list[dict]:
    out = []
    for j in jobs:
        p = {"id": j["id"], "name": j["name"], "arrival": j["arrival_time"], "burst": j["burst_time"],
             "priority": j["priority"], "cpu": j["cpu_cores"], "ram_gb": j["ram_gb"], "storage_gb": j["storage_gb"]}
        if j["queue_level"] is not None:
            p["queue"] = j["queue_level"]
        out.append(p)
    return out


def _select_jobs(tx, user_id: int, job_ids) -> list[dict]:
    if job_ids:
        rows = jobs_repo.get_many(tx, user_id, job_ids)
        if len(rows) != len(set(job_ids)):
            raise not_found("One or more of the selected jobs")     # also hides other users' ids
        if any(r["status"] == states.CANCELLED for r in rows):
            raise conflict("Cancelled jobs cannot be scheduled; remove them from the selection")
    else:
        rows = jobs_repo.schedulable(tx, user_id)
    if not rows:
        raise unprocessable("There are no jobs to schedule. Add a job or load the sample workload first.")
    return rows


def _call_engine(ctx, algorithm: str, processes: list[dict], config: dict) -> dict:
    result = ctx.engine.call({"command": "schedule", "algorithm": algorithm, "processes": processes, "config": config})
    if not result.get("ok"):
        raise unprocessable("The scheduling engine rejected the input", result.get("errors"))
    if len(result.get("segments", [])) > MAX_SEGMENTS:
        raise unprocessable(f"This run would produce more than {MAX_SEGMENTS} timeline segments; "
                            "use a larger time quantum or fewer/shorter jobs")
    return result


def _none(v):
    return None if v is None or v == -1 else v


# ------------------------------------------------------------------ saving
def _replay_allocations(tx, resource_rows: dict, allocations: list[dict]) -> None:
    """Applies the engine's allocate/release timeline to the `resources` table, in time order (releases before
    allocations at equal times). The rows are locked by the caller. The CHECK constraints make the database itself
    refuse any step that would go negative or above capacity, and the totals end at their starting value."""
    steps = []
    for a in allocations:
        if a["server_id"] not in resource_rows:
            continue
        released = _none(a.get("released_at"))
        if released is None:
            raise ApiError(502, "engine_failed", "The engine reported an allocation that was never released")
        d = (a["cpu"], a["ram_gb"], a["storage_gb"])
        steps.append((a["allocated_at"], 1, a["server_id"], d))
        steps.append((released, 0, a["server_id"], tuple(-x for x in d)))
    steps.sort(key=lambda s: (s[0], s[1]))
    used = {rid: [0, 0, 0] for rid in resource_rows}
    for _, _, rid, d in steps:
        cur = used[rid]
        for i in range(3):
            cur[i] += d[i]
        r = resource_rows[rid]
        if (cur[0] > r["total_cpu"] or cur[1] > r["total_ram_gb"] or cur[2] > r["total_storage_gb"]
                or min(cur) < 0):
            raise ApiError(502, "engine_failed", "The engine's allocation timeline exceeds a server's capacity")
        resources_repo.apply_delta(tx, rid, *d)
    if any(any(v != 0 for v in u) for u in used.values()):
        raise ApiError(502, "engine_failed", "The engine's allocations and releases do not balance")


def save_run(tx, user_id: int, algorithm: str, params: dict, jobs: list[dict], servers: list[dict],
             config: dict, result: dict, comparison_id: int | None = None) -> int:
    names = {j["id"]: j["name"] for j in jobs}
    job_by_id = {j["id"]: j for j in jobs}
    snapshot = {"jobs": [{k: j[k] for k in ("id", "name", "arrival_time", "burst_time", "priority", "queue_level",
                                              "cpu_cores", "ram_gb", "storage_gb")} for j in jobs],
                "servers": [{k: s[k] for k in ("id", "name", "total_cpu", "total_ram_gb", "total_storage_gb")} for s in servers],
                "engine_config": config}
    run_id = runs_repo.create(tx, user_id, algorithm, params, bool(params.get("use_cluster", True)), snapshot, comparison_id)

    # 1. lock the server rows (serialises concurrent runs) and make sure the capacities the engine used are current
    locked = {r["id"]: r for r in resources_repo.list_all(tx, lock=True)}
    if params.get("use_cluster", True):
        for s in servers:
            cur = locked.get(s["id"])
            if (cur is None or cur["total_cpu"] != s["total_cpu"] or cur["total_ram_gb"] != s["total_ram_gb"]
                    or cur["total_storage_gb"] != s["total_storage_gb"]):
                raise conflict("Server capacities changed while the run was being calculated; please run it again")

    # 2. per-job results
    procs = result["processes"]
    rows = []
    for p in procs:
        rows.append((p["id"], p["id"], names.get(p["id"], p["name"]), p["arrival"], p["burst"], p["priority"],
                     p["status"], p.get("reason") or None, _none(p.get("server_id")), _none(p.get("admitted_at")),
                     _none(p.get("first_start")), _none(p.get("completion")), _none(p.get("turnaround")),
                     _none(p.get("waiting")), _none(p.get("response"))))
    runs_repo.insert_results(tx, run_id, rows)

    seg_rows = []
    for i, s in enumerate(result["segments"]):
        pid = _none(s.get("process_id"))
        seg_rows.append((i, pid, names.get(pid) if pid is not None else None, max(0, s.get("server_id", 0)),
                         s["start"], s["end"], s["type"], s.get("end_reason") or None, _none(s.get("queue_level"))))
    runs_repo.insert_segments(tx, run_id, seg_rows)

    allocs = result.get("allocations", [])
    runs_repo.insert_allocations(tx, run_id, [
        (a["process_id"], a["process_id"], names.get(a["process_id"]),
         a["server_id"] if a["server_id"] in locked else None, a["server_id"], a["cpu"], a["ram_gb"], a["storage_gb"],
         a["allocated_at"], _none(a.get("released_at")), "RELEASED" if _none(a.get("released_at")) is not None else "ALLOCATED")
        for a in allocs])
    if params.get("use_cluster", True):
        _replay_allocations(tx, locked, allocs)

    # 3. job states: every step of the path is validated against the state machine
    waited = {e["process_id"] for e in result.get("events", []) if e["type"] == "INSUFFICIENT_RESOURCES"}
    event_rows = []
    for p in procs:
        job = job_by_id.get(p["id"])
        if job is None:
            continue
        fresh = jobs_repo.get(tx, user_id, job["id"], lock=True)
        if fresh is None or fresh["status"] == states.CANCELLED:
            continue                                  # cancelled while the run was being saved: leave it alone
        start = fresh["status"] if fresh["status"] in (states.NEW, states.COMPLETED, states.FAILED) else states.NEW
        path = states.run_path(start, p["status"], p["id"] in waited)
        final = path[-1]
        reason = p.get("reason") or None if final == states.FAILED else None
        jobs_repo.set_status(tx, user_id, job["id"], final, reason)
        event_rows.append((user_id, run_id, job["id"], "JOB_STATE_CHANGE",
                           f"Job '{job['name']}': {start} -> " + " -> ".join(path), None))

    # 4. event log: engine events + placement decision log (capped)
    engine_rows = [(user_id, run_id, _job_ref(e["process_id"], job_by_id), e["type"], e["description"], e["time"])
                   for e in result.get("events", [])]
    decision_rows = [(user_id, run_id, _job_ref(d.get("process_id"), job_by_id), "PLACEMENT_DECISION",
                      d["message"].strip(), d.get("time")) for d in result.get("decision_log", [])]
    stored = (engine_rows + decision_rows)[:MAX_STORED_EVENTS]
    if len(engine_rows) + len(decision_rows) > MAX_STORED_EVENTS:
        stored.append((user_id, run_id, None, "LOG_TRUNCATED",
                       f"Only the first {MAX_STORED_EVENTS} log lines were stored for this run", None))
    events_repo.add_many(tx, event_rows + stored)

    m = result["metrics"]
    runs_repo.mark_completed(tx, run_id, {k: m[k] for k in METRIC_KEYS})
    events_repo.add(tx, user_id, "RUN_COMPLETED",
                    f"{algorithm} run #{run_id}: {m['completed']} completed, {m['rejected']} rejected, "
                    f"avg waiting {m['avg_waiting']}, avg turnaround {m['avg_turnaround']}", run_id=run_id)
    return run_id


def _job_ref(pid, job_by_id):
    return pid if pid in job_by_id else None


# ------------------------------------------------------------------ public operations
def _prepare(ctx, user_id: int, params: dict):
    with ctx.db.transaction() as tx:
        jobs = _select_jobs(tx, user_id, params.get("job_ids"))
        servers = resources_repo.list_all(tx)
    if params.get("use_cluster", True) and not servers:
        raise unprocessable("No servers are configured (the resources table is empty). Run the seed migration.")
    return jobs, servers


def run(ctx, user_id: int, req: dict) -> dict:
    algorithm = req["algorithm"]
    jobs, servers = _prepare(ctx, user_id, req)
    config = engine_config(req, servers)
    result = _call_engine(ctx, algorithm, engine_processes(jobs), config)
    try:
        with ctx.db.transaction() as tx:
            run_id = save_run(tx, user_id, algorithm, _params_only(req), jobs, servers, config, result)
    except ApiError:
        raise
    except ctx.db.integrity_errors as exc:
        raise conflict("The run could not be saved because the data changed concurrently; please retry") from exc
    return detail(ctx, user_id, run_id)


def _params_only(req: dict) -> dict:
    keep = ("time_quantum", "mlq_queues", "mlfq_quanta", "mlfq_aging", "use_cluster", "job_ids")
    return {k: req.get(k) for k in keep}


def compare(ctx, user_id: int, req: dict) -> dict:
    jobs, servers = _prepare(ctx, user_id, req)
    config = engine_config(req, servers)
    processes = engine_processes(jobs)
    results = {alg: _call_engine(ctx, alg, processes, config) for alg in req["algorithms"]}
    ranking = ctx.engine.call({"command": "rank", "items": [
        {"label": alg, "metrics": {k: r["metrics"][k] for k in METRIC_KEYS}} for alg, r in results.items()]})
    if not ranking.get("ok"):
        raise unprocessable("The engine could not rank the results", ranking.get("errors"))
    params = _params_only(req)
    with ctx.db.transaction() as tx:     # one transaction: either every run and the comparison are saved, or none
        comparison_id = runs_repo.create_comparison(tx, user_id, req["name"], list(req["algorithms"]), params)
        run_ids = [save_run(tx, user_id, alg, params, jobs, servers, config, r, comparison_id)
                   for alg, r in results.items()]
        runs_repo.set_comparison_best(tx, comparison_id, ranking["best"])
        events_repo.add(tx, user_id, "COMPARISON_SAVED",
                        f"Comparison '{req['name']}' saved with {len(run_ids)} runs", run_id=None)
    return comparison_detail(ctx, user_id, comparison_id)


# ------------------------------------------------------------------ reading
def _present_result(r: dict) -> dict:
    return {k: r[k] for k in ("job_id", "process_ref", "job_name", "arrival_time", "burst_time", "priority",
                              "result_status", "reason", "server_ref", "admitted_at", "first_start",
                              "completion_time", "turnaround_time", "waiting_time", "response_time")}


def _present_alloc(a: dict) -> dict:
    from ..db import iso
    return {"id": a["id"], "run_id": a["run_id"], "job_id": a["job_id"], "process_ref": a["process_ref"],
            "job_name": a["job_name"], "resource_id": a["resource_id"], "server_ref": a["server_ref"],
            "cpu_cores": a["cpu_cores"], "ram_gb": a["ram_gb"], "storage_gb": a["storage_gb"],
            "allocated_at": a["allocated_at"], "released_at": a["released_at"], "status": a["alloc_status"],
            "created_at": iso(a.get("created_at"))}


def _require_run(tx, user_id: int, run_id: int) -> dict:
    row = runs_repo.get(tx, user_id, run_id)
    if row is None:
        raise not_found("Run")
    return row


def detail(ctx, user_id: int, run_id: int) -> dict:
    with ctx.db.transaction() as tx:
        row = _require_run(tx, user_id, run_id)
        results = [_present_result(r) for r in runs_repo.results(tx, run_id)]
        segments = runs_repo.segments(tx, run_id)
        allocs = [_present_alloc(a) for a in runs_repo.allocations_for_run(tx, run_id)]
    return {"run": runs_repo.present_run(row), "results": results, "gantt": _gantt(segments), "allocations": allocs}


def _gantt(segments: list[dict]) -> list[dict]:
    return [{"seq": s["seq"], "process_ref": s["process_ref"], "job_name": s["job_name"], "server_ref": s["server_ref"],
             "start": s["start_time"], "end": s["end_time"], "type": s["segment_type"], "end_reason": s["end_reason"],
             "queue_level": s["queue_level"]} for s in segments]


def gantt(ctx, user_id: int, run_id: int) -> dict:
    with ctx.db.transaction() as tx:
        row = _require_run(tx, user_id, run_id)
        segs = runs_repo.segments(tx, run_id)
    return {"run_id": run_id, "algorithm": row["algorithm"], "segments": _gantt(segs)}


def results(ctx, user_id: int, run_id: int) -> dict:
    with ctx.db.transaction() as tx:
        row = _require_run(tx, user_id, run_id)
        res = runs_repo.results(tx, run_id)
    return {"run": runs_repo.present_run(row), "results": [_present_result(r) for r in res]}


def list_runs(ctx, user_id: int, **filters) -> dict:
    with ctx.db.transaction() as tx:
        rows, total = runs_repo.search(tx, user_id, **filters)
    return {"items": [runs_repo.present_run(r) for r in rows], "total": total,
            "page": max(1, filters["page"]), "page_size": min(max(1, filters["page_size"]), 200)}


def delete_run(ctx, user_id: int, run_id: int) -> None:
    with ctx.db.transaction() as tx:
        if runs_repo.delete(tx, user_id, run_id) == 0:
            raise not_found("Run")
        events_repo.add(tx, user_id, "RUN_DELETED", f"Run #{run_id} deleted")


def comparison_detail(ctx, user_id: int, comparison_id: int) -> dict:
    with ctx.db.transaction() as tx:
        row = runs_repo.get_comparison(tx, user_id, comparison_id)
        if row is None:
            raise not_found("Comparison")
        run_rows, _ = runs_repo.search(tx, user_id, algorithm=None, status=None, comparison_id=comparison_id,
                                       sort="id", order="asc", page=1, page_size=200)
    return {"comparison": runs_repo.present_comparison(row), "runs": [runs_repo.present_run(r) for r in run_rows]}


def list_comparisons(ctx, user_id: int, page: int, page_size: int) -> dict:
    with ctx.db.transaction() as tx:
        rows, total = runs_repo.list_comparisons(tx, user_id, page, page_size)
    return {"items": [runs_repo.present_comparison(r) for r in rows], "total": total, "page": max(1, page),
            "page_size": min(max(1, page_size), 200)}


def compare_saved(ctx, user_id: int, run_ids: list[int]) -> dict:
    """Ranks already-saved runs (any algorithms/inputs) with the engine's `rank` command."""
    if len(set(run_ids)) != len(run_ids) or not 2 <= len(run_ids) <= 20:
        raise unprocessable("Choose between 2 and 20 different runs to compare")
    with ctx.db.transaction() as tx:
        rows = runs_repo.get_many(tx, user_id, run_ids)
    if len(rows) != len(run_ids):
        raise not_found("One or more of the selected runs")
    if any(r["status"] != "COMPLETED" for r in rows):
        raise conflict("Only completed runs can be compared")
    runs = [runs_repo.present_run(r) for r in rows]
    ranking = ctx.engine.call({"command": "rank", "items": [
        {"label": f"#{r['id']} {r['algorithm']}", "metrics": {k: r["metrics"][k] for k in METRIC_KEYS}} for r in runs]})
    if not ranking.get("ok"):
        raise unprocessable("The engine could not rank the runs", ranking.get("errors"))
    return {"runs": runs, "best_by_metric": ranking["best"]}


def utilization_series(ctx, user_id: int, run_id: int) -> dict:
    """Per-server utilisation over simulated time, derived from the stored allocation rows of the run."""
    with ctx.db.transaction() as tx:
        _require_run(tx, user_id, run_id)
        allocs = runs_repo.allocations_for_run(tx, run_id)
        servers = {s["id"]: s for s in resources_repo.list_all(tx)}
    times = sorted({a["allocated_at"] for a in allocs} | {a["released_at"] for a in allocs if a["released_at"] is not None})
    series = []
    for t in times:
        per = {}
        for a in allocs:
            if a["allocated_at"] <= t and (a["released_at"] is None or t < a["released_at"]):
                u = per.setdefault(a["server_ref"], [0, 0, 0])
                u[0] += a["cpu_cores"]; u[1] += a["ram_gb"]; u[2] += a["storage_gb"]
        entry = []
        for sid, s in sorted(servers.items()):
            u = per.get(sid, [0, 0, 0])
            entry.append({"server_id": sid, "name": s["name"],
                          "cpu_percent": round(100 * u[0] / s["total_cpu"], 2),
                          "ram_percent": round(100 * u[1] / s["total_ram_gb"], 2),
                          "storage_percent": round(100 * u[2] / s["total_storage_gb"], 2)})
        series.append({"time": t, "servers": entry})
    return {"run_id": run_id, "series": series}
