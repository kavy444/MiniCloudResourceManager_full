# C++ engine protocol (`mcrm_engine`)

`build/mcrm_engine(.exe)` reads **one JSON object from stdin**, writes **one JSON object to stdout** and exits (exit code 0 even
when it reports `{"ok": false}`; non-zero = crash). The Python backend starts a fresh process per request (`backend/app/engine.py`),
so the engine keeps no state. Try it by hand:

```powershell
'{"command":"info"}' | .\build\mcrm_engine.exe
```

Every request has `"command"`. Every response has `"ok"`; on failure: `{"ok": false, "errors": ["..."]}` (all problems are listed).
Limits: 1000 processes, burst <= 100000, time values <= 1000000, <= 8 queues, <= 16 servers.

| command | request fields | response (on success) |
|---|---|---|
| `info` | - | engine version, algorithm names, limits |
| `schedule` | `algorithm`, `processes[]`, `config` | `metrics`, `processes[]`, `segments[]`, `events[]`, `allocations[]`, `samples[]`, `server_summaries[]`, `decision_log[]` |
| `compare` | `algorithms[]`, `processes[]`, `config` | `results[]`, `best[]` |
| `rank` | `items: [{label, metrics}]` | `best[]`: best label(s) per metric, ties and "all equal" flags |
| `place` | `servers[]` (with used amounts), `job` | the chosen server and an explanation per server |
| `memory` | `strategy` (`FIRST_FIT`/`BEST_FIT`/`WORST_FIT`), `total_size` or `partitions[]`, `operations[]` | `steps[]` with memory maps, `final_stats` |
| `banker` | `action` (`safety`/`request`/`detect`), `available`, `max`, `allocation`, `process`, `request` | safety result / grant decision / deadlock result |
| `sync` | `scenario` (`race`/`producer_consumer`), options | `steps[]`, counters, `deadlock`, `summary` |

## `schedule` request

```json
{
  "command": "schedule",
  "algorithm": "RR",
  "processes": [
    {"id": 1, "name": "web", "arrival": 0, "burst": 6, "priority": 2, "queue": -1,
     "cpu": 2, "ram_gb": 4, "storage_gb": 20}
  ],
  "config": {
    "time_quantum": 2,
    "mlq":  [{"policy": "RR", "quantum": 2}, {"policy": "RR", "quantum": 4}, {"policy": "FCFS", "quantum": 0}],
    "mlfq": {"quanta": [2, 4, 8], "aging": 0},
    "cluster": {"enabled": true,
                "servers": [{"id": 1, "name": "Server-1", "cpu": 8, "ram_gb": 32, "storage_gb": 500}]}
  }
}
```

* `algorithm`: `FCFS SJF PRIORITY_NP HRRN SRTF RR PRIORITY_P MLQ MLFQ`.
* `priority`: smaller number = more urgent. `queue`: MLQ queue index, `-1` = derive from priority (`priority-1`, clamped).
* `cluster.enabled = false` (or no cluster) = plain single-CPU scheduling; resource fields are ignored.
* The backend uses the job's database id as `id` and the `resources.id` as the server `id`.

## `schedule` response (abridged)

```json
{
  "ok": true, "algorithm": "RR",
  "metrics": {"total_processes": 3, "completed": 2, "rejected": 1, "avg_waiting": 1.5, "avg_turnaround": 5.0,
              "avg_response": 1.5, "total_time": 7, "start_time": 0, "throughput": 0.285714,
              "cpu_utilization": 100.0, "context_switches": 1, "preemptions": 0},
  "processes": [{"id": 1, "name": "a", "arrival": 0, "burst": 4, "priority": 1, "status": "COMPLETED", "reason": "",
                 "server_id": 7, "admitted_at": 0, "first_start": 0, "completion": 4, "turnaround": 4,
                 "waiting": 0, "response": 0}],
  "segments": [{"process_id": 1, "server_id": 7, "start": 0, "end": 4, "type": "EXEC",
                "end_reason": "COMPLETED", "queue_level": -1}],
  "events": [{"time": 0, "type": "RESOURCES_ALLOCATED", "process_id": 1, "server_id": 7, "description": "..."}],
  "allocations": [{"process_id": 1, "server_id": 7, "cpu": 2, "ram_gb": 4, "storage_gb": 10, "allocated_at": 0, "released_at": 4}],
  "decision_log": [{"time": 0, "process_id": 1, "message": "Job 1: Selected S: lowest load score 0.132 ..."}]
}
```

`-1` in a numeric field means "not applicable" (for example a rejected job has no completion time). Segment `type` is `EXEC` or
`IDLE`; `end_reason` is `COMPLETED`, `PREEMPTED` or `QUANTUM_EXPIRED`. Event types: `JOB_ARRIVED`, `RESOURCES_ALLOCATED`,
`JOB_STARTED`, `INSUFFICIENT_RESOURCES`, `JOB_REJECTED`, `JOB_PREEMPTED`, `JOB_DEMOTED`, `JOB_PROMOTED`, `JOB_COMPLETED`,
`RESOURCES_RELEASED`.
