# Mini Cloud Resource Manager (OS + DBMS project)

A **simulation** of a small cloud cluster: users log in, create jobs, run nine CPU-scheduling algorithms on three simulated servers,
and inspect Gantt charts, metrics, resource allocation history, decision logs and operating-system teaching modules.
Nothing here is a real cloud. All results are produced by a C++17 engine and stored in MySQL.

```
 Browser (HTML/CSS/vanilla JS)  --HTTP/JSON-->  FastAPI backend (Python)  --stdin/stdout JSON-->  C++ engine (mcrm_engine)
                                                       |
                                                       +--parameterized SQL-->  MySQL 8  (database mini_cloud_manager)
```

* The **browser never talks to MySQL**. It only calls the backend API (`/api/...`) with a bearer token.
* **All scheduling logic is C++** (`src/`, `include/`). Python only validates requests, starts the engine, and stores the results.
* The database tables are created by SQL migration files (`backend/migrations/`), not by application code.

## Status — what is implemented, and what has been verified

| Area | Implemented | How it was verified |
|---|---|---|
| 9 scheduling algorithms (FCFS, SJF, Priority NP, HRRN, SRTF, Round Robin, Priority preemptive, MLQ, MLFQ), configurable quantum / queues / aging | yes, C++ | 48 C++ engine tests (20,518 checks) incl. hand-calculated schedules |
| Cluster model: 3 servers, least-loaded placement, waiting, retry on release, rejection | yes, C++ | 2,102 checks (milestone-1 suite) + engine tests |
| Memory allocation (first/best/worst fit), Banker's algorithm, deadlock detection, race / producer-consumer demos | yes, C++ | engine tests (textbook examples) |
| Backend services, repositories, auth, validation, run saving in one transaction | yes, Python | 50 unit/service tests on **SQLite** with the real migrations and the real C++ engine |
| HTTP API (FastAPI) | yes | 18 HTTP tests + 43 browser checks, but run against a **small Starlette-based stand-in for FastAPI** (see "Not verified") |
| Frontend (dashboard, jobs, scheduler, Gantt, analytics, event log, OS modules) | yes | headless Chromium (Playwright) against the stand-in server |
| **Real MySQL 8** and **real FastAPI/PyMySQL** | written for them | **not executed in the build environment** (no MySQL server, package downloads blocked). See "Not verified". |

### Not verified (please run these yourself)

The environment this project was written in could not install FastAPI, PyMySQL or MySQL. So these were **not** executed:

1. The migrations on a real MySQL server (they were run on SQLite after a mechanical translation; they avoid MySQL-only syntax where possible,
   but MySQL may still report an error — if it does, the message names the statement).
2. Row locking (`SELECT ... FOR UPDATE`) and real concurrent transactions on InnoDB. The SQLite tests prove the *logic*
   (balance, rollback, constraints) but not MySQL's lock behaviour.
3. The app under the real `fastapi` package. The code sticks to a conservative FastAPI subset (typed parameters, `Depends`, one Pydantic body model),
   and the HTTP tests will run automatically against real FastAPI when you run them (`python -m unittest discover -s tests -t .` inside `backend/`).

Please run the test commands below on your machine and treat any failure as a bug to report.

## Prerequisites (Windows 10/11, VS Code)

| Tool | Why | Check |
|---|---|---|
| **MinGW-w64 g++ 9 or newer** (MSYS2 `mingw-w64-ucrt-x86_64-gcc`, or WinLibs) with `g++` on `PATH` | build the C++ engine | `g++ --version` |
| **Python 3.10+** | backend | `python --version` |
| **MySQL 8** (already installed on your machine) | database `mini_cloud_manager`, user `cloud_app` (already created) | `mysql --version` |
| VS Code + extensions: *C/C++*, *Python* | editing / tasks | |

macOS/Linux work too: use `make` instead of `build_windows.bat`, and `python3`.

## First-time setup (PowerShell, from the project folder)

```powershell
# 1. Build the C++ engine and its tests (about a minute)
.\build_windows.bat
.\build\engine_tests.exe          # expect: "... 0 failed tests"
.\build\tests.exe                 # milestone-1 console simulator tests

# 2. Python environment
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r backend\requirements.txt

# 3. Configuration: copy the template, then edit backend\.env in VS Code
copy backend\.env.example backend\.env
```

Edit `backend\.env` **yourself**: put your real `DB_PASSWORD` there and generate a long random `JWT_SECRET`
(`python -c "import secrets; print(secrets.token_urlsafe(48))"`). `backend\.env` is listed in `.gitignore`; never commit it, never paste it into chat.
The password is read only by the backend process; it never reaches the browser, the SQL files or the documentation.

```powershell
# 4. Create the tables and the three servers (uses the application account cloud_app, NOT root)
python backend\scripts\migrate.py
```

The database and the user already exist, so nothing is recreated. If `migrate.py` says *access denied* or *command denied*, the
`cloud_app` account lacks privileges. In the MySQL shell, as an administrator, run (adjust the host part if needed):

```sql
GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, ALTER, INDEX, REFERENCES ON mini_cloud_manager.* TO 'cloud_app'@'localhost';
FLUSH PRIVILEGES;
```

(After the tables exist you may revoke `CREATE, ALTER, INDEX, REFERENCES` again; normal operation needs only SELECT, INSERT, UPDATE, DELETE.)

```powershell
# 5. Optional: a demo account with the labelled sample jobs (asks for a password at the prompt)
python backend\scripts\seed_demo.py

# 6. Start the backend (it also serves the frontend)
cd backend
python -m uvicorn app.main:app --host 127.0.0.1 --port 8000
```

Open **http://127.0.0.1:8000** and create an account (the first account becomes the administrator, who may edit server capacities).
Interactive API docs: http://127.0.0.1:8000/api/docs. VS Code tasks for every step are in `.vscode/tasks.json` (Terminal > Run Task).

Sanity check that the engine alone works (PowerShell): `'{"command":"info"}' | .\build\mcrm_engine.exe`.

## Using the dashboard

| Page | What you can do |
|---|---|
| **Overview** | job counters by state, the three server cards, the latest run's metrics |
| **Servers** | capacities and availability, utilization-over-time chart for any run, allocation history (filter/paginate), admin: edit capacity |
| **Jobs** | create / edit / cancel / delete jobs (validated in the browser *and* the backend), filter, sort, search, load labelled sample jobs |
| **Scheduler** | pick an algorithm and its settings (quantum, MLQ queues, MLFQ quanta and aging, cluster model on/off, which jobs), run it, see metrics, Gantt chart, per-job table and the decision log |
| **Analytics** | compare several algorithms on the same jobs (best algorithm per metric), browse/sort/filter saved runs, compare saved runs, saved comparisons |
| **Event log** | filter by type, run, job, text and date range |
| **OS modules** | memory allocation, Banker's algorithm / deadlock detection (stored in the database), synchronization demos |

Everything shown comes from the backend. The only pre-made data are the **sample jobs** you load explicitly; they carry a "sample" label.

## How the system works

### Request flow for "Run scheduler"
1. Browser: `POST /api/runs {algorithm, time_quantum, ...}` with the bearer token.
2. Backend: validates the body (Pydantic), loads *your* jobs and the server capacities from MySQL.
3. Backend starts `mcrm_engine`, sends one JSON request, receives one JSON answer (the schedule, Gantt segments, events, allocations, metrics).
4. Backend saves everything in **one transaction**: run row, per-job results, execution segments, allocations, job state changes, events.
   It locks the `resources` rows, replays the allocations/releases in time order against them (CHECK constraints forbid going below 0 or above capacity),
   and the totals end at their starting value. Any error rolls the whole run back.
5. Browser draws the Gantt chart from the stored segments.

### The scheduling model
* **Time** is an integer number of abstract *time units*. The clock starts at the earliest arrival and jumps from event to event
  (arrival, completion, quantum expiry, aging deadline); it is deterministic.
* **Two separate decisions.** *Admission/placement* (cluster model): which server gets the job, and does it fit (CPU + RAM + storage)?
  *CPU scheduling*: of the jobs admitted to a server, which runs now? Each server has one logical CPU, so jobs on different servers overlap in time.
  With the cluster model **off** there is a single CPU and resources are ignored (the textbook model).
* **Priority:** a **smaller number is more urgent** (1 = High, 2 = Medium, 3 = Low in the UI). Ties are broken by arrival order, then job id.

| Algorithm | Preemptive | Rule (ties: arrival order, then id) |
|---|---|---|
| FCFS | no | earliest arrival first |
| SJF | no | smallest burst first |
| PRIORITY_NP | no | best (smallest) priority number first |
| HRRN | no | largest response ratio (waiting + burst) / burst first |
| SRTF | yes | smallest *remaining* time; a shorter arrival preempts; equal remaining keeps the running job |
| RR | yes | FIFO queue, each turn at most `time_quantum` |
| PRIORITY_P | yes | like PRIORITY_NP, but a more urgent arrival preempts |
| MLQ | per queue | fixed queues (0 = highest). Each queue has its own policy (FCFS/RR/SJF/PRIORITY, RR has a quantum). A job's queue is its `queue_level`, or `priority-1` (clamped) if empty. Strict priority between queues |
| MLFQ | yes | all jobs start in level 0; using a full quantum demotes a job; the last level runs to completion (quantum 0). Optional **aging** promotes a job that waited too long |

### Metrics (averages are over completed jobs only)
* **Turnaround** = completion − arrival. **Waiting** = turnaround − burst. **Response** = first start − arrival.
* **Total time** = last completion − earliest arrival. **Throughput** = completed jobs / total time.
* **CPU utilization** = busy time / (number of servers × total time). **Context switches** = times the CPU changed from one job to a different job.

### Server placement (least-loaded)
A server is *eligible* only if the job's CPU, RAM and storage all fit in what is free. For each eligible server the engine computes
`score = (used_cpu+req_cpu)/total_cpu + (used_ram+req_ram)/total_ram + (used_storage+req_storage)/total_storage` divided by 3 —
the average utilization **after** placement. The lowest score wins; ties go to the lowest server id. The decision log shows the score of every
eligible server and, for rejected servers, which resource was short.

### Job states
| State | Meaning |
|---|---|
| NEW | saved, never scheduled (also after an edit) |
| READY | selected for a run, awaiting admission/dispatch (the engine calls this *queued*) |
| WAITING | valid, **but the cluster has no free capacity right now**; it is retried whenever resources are released |
| RUNNING | resources reserved, executing |
| COMPLETED | finished; resources released exactly once |
| FAILED | rejected (larger than every server) or aborted — see the reason |
| CANCELLED | withdrawn by the user; never scheduled again (final) |

*Queued vs waiting:* a QUEUED/READY job has simply not been looked at yet; a WAITING job **has** been looked at, is allowed to run, and is blocked only by missing resources.
Allowed transitions: NEW→READY, READY→RUNNING/WAITING/FAILED, WAITING→RUNNING/FAILED, RUNNING→COMPLETED/FAILED, COMPLETED/FAILED→READY (re-run),
READY/WAITING/COMPLETED/FAILED→NEW (edit), NEW/READY/WAITING→CANCELLED; a running job cannot be edited or deleted. A run is a self-contained simulation, so the intermediate states are *passed through* while the run is saved
(every step is validated against this table, and a transition log line is written); the stored final state is COMPLETED or FAILED.

## Database (MySQL, `backend/migrations/001_schema.sql`)

| Table | Purpose / key columns |
|---|---|
| `users` | username (unique), scrypt password hash, role (user/admin) |
| `jobs` | owner `user_id`, name, arrival, burst, priority, queue_level, cpu/ram/storage, status, status_reason, `is_sample` |
| `resources` | the 3 servers: totals, `allocated_*` (CHECK 0 ≤ allocated ≤ total) |
| `scheduling_runs` | one row per run: algorithm, params (JSON), input snapshot (JSON), aggregate metrics |
| `scheduling_results` | per-job results of a run (snapshot of name/arrival/burst + waiting/turnaround/response) |
| `execution_segments` | the Gantt segments (EXEC/IDLE, end reason, queue level) |
| `resource_allocations` | per-job allocate/release records (ALLOCATED/RELEASED consistent with `released_at`) |
| `system_events` | persisted event/decision log |
| `deadlock_checks` | Banker's / detection checks with inputs, outputs and safe sequence |
| `algorithm_comparisons` | saved comparison; its runs point to it |
| `schema_migrations` | which migration files were applied |

Relationships and delete rules: `jobs.user_id → users` (RESTRICT) · `scheduling_runs.user_id → users` (RESTRICT) · results/segments/allocations `→ scheduling_runs` (**CASCADE**, deleting a run deletes its details) ·
`results.job_id`, `allocations.job_id/resource_id`, `system_events.*` (**SET NULL**: history survives deleting a job, because results keep their own copy of the job's data) ·
`scheduling_runs.comparison_id → algorithm_comparisons` (SET NULL). Indexes cover every foreign key and the common filters (user + status, user + created time, run + sequence).

Every query is parameterized (`%s` placeholders; sort columns come from fixed whitelists). Each request uses a transaction from a bounded connection pool.

## API (all under `/api`, JSON; every endpoint except `health`, `auth/register`, `auth/login` needs `Authorization: Bearer <token>`)

| Method & path | Purpose |
|---|---|
| `GET /health` | database + engine status |
| `POST /auth/register`, `POST /auth/login`, `GET /auth/me` | accounts and tokens |
| `GET /jobs` (`status,q,sort,order,page,page_size`), `POST /jobs`, `GET/PUT/DELETE /jobs/{id}`, `POST /jobs/{id}/cancel`, `POST /jobs/samples` | job CRUD |
| `POST /runs` | run an algorithm and save it |
| `GET /runs` (`algorithm,status,comparison_id,sort,order,page,page_size`), `GET /runs/{id}`, `DELETE /runs/{id}` | history |
| `GET /runs/{id}/results`, `/gantt`, `/utilization` | results, Gantt segments, utilization over time |
| `POST /runs/compare-saved` | rank 2–20 saved runs |
| `POST /comparisons`, `GET /comparisons`, `GET /comparisons/{id}` | run + save several algorithms at once |
| `GET /resources`, `PUT /resources/{id}` (admin) | servers |
| `GET /allocations` (`run_id,job_id,status,server,order,page,page_size`) | allocation history |
| `GET /events` (`event_type,run_id,job_id,q,since,until,order,page,page_size`) | event log |
| `GET /dashboard`, `GET /meta/algorithms` | overview data, algorithm catalog |
| `POST /os/memory`, `POST /os/banker`, `POST /os/sync`, `GET /os/deadlock-checks` | OS modules |

Errors always have the shape `{"error": {"code": "...", "message": "...", "details": [...]}}`: 400/422 invalid input, 401 not logged in, 403 not allowed,
404 not found (also used for other users' objects), 409 conflict, 429 too many login attempts, 503/504 engine or database unavailable.

## Security — what is and is not done

Done: scrypt password hashing with per-user salts; JWT bearer tokens that carry only the user id (the user and role are reloaded from MySQL on every request);
the user id for authorization always comes from the token, never from the request body (bodies with unknown fields are rejected);
every query filters by the owner, and other users' objects answer 404; parameterized SQL; strict request validation with size limits;
login throttling (5 failures / 5 minutes per username+address); CORS only for origins you list in `CORS_ORIGINS` (never `*`); security headers; internal errors are logged on the server and never shown to the client;
the browser inserts server data as text, never as HTML; secrets only in `backend/.env`; the app uses `cloud_app`, not `root`.

**This is not a claim that the system is secure.** There is no HTTPS (run behind a TLS proxy if you ever expose it), no password reset, no email verification,
the login limiter is in memory (resets on restart), tokens cannot be revoked before they expire (8 h by default), and there has been no security audit or penetration test.
Use it on your own machine / college network only.

## Tests

```powershell
.\build\engine_tests.exe                                  # C++ engine: 48 tests
.\build\tests.exe                                         # milestone-1 console simulator: 2,102 checks
cd backend; python -m unittest discover -s tests -t .      # Python: 68 tests (18 HTTP tests need fastapi + uvicorn, else they skip)
```
Linux/macOS: `make test` runs all three.

### Test report (as run while building this project)

| Suite | Result |
|---|---|
| C++ milestone-1 tests (`tests/test_main.cpp`) | 2,102 checks, 0 failures |
| C++ engine tests (`tests/engine/`) | 48 tests, 20,518 checks, 0 failures |
| Python service/core tests on SQLite + real engine | 50 tests, all passed |
| Python HTTP tests (uvicorn + Starlette-based stand-in for FastAPI) | 18 tests, all passed |
| Browser end-to-end (Playwright / Chromium against the stand-in): login, validation errors, jobs, scheduling, Gantt, comparison, filters, OS modules, XSS check, phone-width layout | 43 checks, all passed |

What the tests cover: every algorithm on hand-calculated workloads (waiting/turnaround/response, throughput, utilization, context switches, Gantt order);
equal priorities and ties; empty workload; invalid input; a job larger than every server (REJECTED with reason); insufficient capacity → WAITING → retried after release;
resources return to zero after every run and never go negative or above capacity (also enforced by the database CHECK constraints); idempotent migrations;
state-machine transitions (legal and illegal); per-user isolation on every endpoint; login throttling; tampered/expired tokens; SQL-injection-shaped input; concurrent runs;
Banker's algorithm (textbook example, request granted/denied, deadlock detection); memory strategies; synchronization demos.

## Console simulator (milestone 1) and workload demo

`build\mcrm.exe` (menu) and `build\mcrm.exe --demo` (repeatable workload) are the original console program; the engine reuses its resource manager.

## Known limitations

* The simulation is deliberately simple: integer time units, one logical CPU per server, no I/O or memory contention between jobs.
* Runs are independent: the cluster is idle at the start of each run (so live utilization between runs is 0%).
* At most 200 jobs per run, burst ≤ 10,000 time units, 20,000 Gantt segments per run (use a larger quantum for huge workloads).
* The job list returns at most 200 rows per page; the Scheduler job picker shows the first 200 jobs.
* No automatic deletion of old runs/events; delete runs from the Analytics page.
* Not verified on real MySQL / real FastAPI (see above).

## Project layout

```
include/ src/          C++: scheduling engine, resource manager, OS modules, JSON, engine_main.cpp (mcrm_engine), main.cpp (console)
tests/                 C++ tests (test_main.cpp = milestone 1; engine/ = engine tests)
backend/app/           FastAPI app: routers/ (HTTP), services/ (logic + transactions), repositories/ (SQL), models.py, security.py, db.py, engine.py
backend/migrations/    001_schema.sql, 002_seed_resources.sql
backend/scripts/       migrate.py, seed_demo.py
backend/tests/         Python tests (test-only SQLite backend in sqlite_backend.py)
frontend/              index.html, style.css, js/ (api, ui, charts, pages/)
docs/ENGINE_PROTOCOL.md   JSON protocol of the C++ engine
build_windows.bat, Makefile, .vscode/tasks.json
```
