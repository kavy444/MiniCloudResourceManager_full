-- Mini Cloud Resource Manager - schema, migration 001
-- Target: MySQL 8.0.16+ (CHECK constraints are enforced from that version), InnoDB, utf8mb4.
-- Run it with:  python scripts/migrate.py        (it reads DB_* from your .env file)
--
-- Style rules (so the file stays readable AND can be exercised by the SQLite based unit tests):
--   * one statement per ';' at the end of a line, no stored programs;
--   * enums are VARCHAR + CHECK, booleans are TINYINT, JSON columns hold JSON text;
--   * constraint names are unique across the whole schema (MySQL requires that for CHECKs);
--   * no CHECK on a column that also has a referential action (MySQL forbids that combination).
--
-- Deletion policy (history is never lost by accident):
--   users               RESTRICT  - a user who owns data cannot be deleted
--   jobs                SET NULL  - deleting a job keeps results/allocations (they store a snapshot of the job)
--   scheduling_runs     CASCADE   - deleting a run deliberately removes its results, segments, allocations
--   system_events       SET NULL  - the audit trail survives deletion of runs/jobs/users
--   resources           SET NULL  - allocation history survives a re-configured/removed server

CREATE TABLE users (
    id            BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    username      VARCHAR(50)  NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    role          VARCHAR(10)  NOT NULL DEFAULT 'user',
    created_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_login_at DATETIME     NULL,
    CONSTRAINT uq_users_username UNIQUE (username),
    CONSTRAINT ck_users_role CHECK (role IN ('user', 'admin'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- The simulated cluster: one row per simulated server. `allocated_*` is changed ONLY inside the transaction
-- that saves a scheduling run, which first locks these rows (SELECT ... FOR UPDATE); the CHECK constraints make
-- it impossible for the database to ever hold more than the capacity or a negative amount.
CREATE TABLE resources (
    id                   BIGINT      NOT NULL AUTO_INCREMENT PRIMARY KEY,
    name                 VARCHAR(50) NOT NULL,
    total_cpu            INT         NOT NULL,
    total_ram_gb         INT         NOT NULL,
    total_storage_gb     INT         NOT NULL,
    allocated_cpu        INT         NOT NULL DEFAULT 0,
    allocated_ram_gb     INT         NOT NULL DEFAULT 0,
    allocated_storage_gb INT         NOT NULL DEFAULT 0,
    updated_at           DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT uq_resources_name UNIQUE (name),
    CONSTRAINT ck_resources_totals CHECK (total_cpu > 0 AND total_ram_gb > 0 AND total_storage_gb > 0),
    CONSTRAINT ck_resources_alloc_min CHECK (allocated_cpu >= 0 AND allocated_ram_gb >= 0 AND allocated_storage_gb >= 0),
    CONSTRAINT ck_resources_alloc_max CHECK (allocated_cpu <= total_cpu AND allocated_ram_gb <= total_ram_gb
                                             AND allocated_storage_gb <= total_storage_gb)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE jobs (
    id            BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id       BIGINT       NOT NULL,
    name          VARCHAR(100) NOT NULL,
    arrival_time  INT          NOT NULL DEFAULT 0,
    burst_time    INT          NOT NULL,
    priority      INT          NOT NULL DEFAULT 2,
    queue_level   INT          NULL,
    cpu_cores     INT          NOT NULL DEFAULT 1,
    ram_gb        INT          NOT NULL DEFAULT 1,
    storage_gb    INT          NOT NULL DEFAULT 1,
    status        VARCHAR(12)  NOT NULL DEFAULT 'NEW',
    status_reason VARCHAR(500) NULL,
    is_sample     TINYINT      NOT NULL DEFAULT 0,
    created_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT fk_jobs_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE RESTRICT,
    CONSTRAINT ck_jobs_times CHECK (arrival_time >= 0 AND burst_time >= 1),
    CONSTRAINT ck_jobs_priority CHECK (priority >= 0),
    CONSTRAINT ck_jobs_demand CHECK (cpu_cores >= 1 AND ram_gb >= 1 AND storage_gb >= 1),
    CONSTRAINT ck_jobs_queue CHECK (queue_level IS NULL OR queue_level >= 0),
    CONSTRAINT ck_jobs_status CHECK (status IN ('NEW', 'READY', 'RUNNING', 'WAITING', 'COMPLETED', 'FAILED', 'CANCELLED'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE INDEX ix_jobs_user_status ON jobs (user_id, status);
CREATE INDEX ix_jobs_user_created ON jobs (user_id, created_at);

-- One saved comparison groups several runs made on the same inputs.
CREATE TABLE algorithm_comparisons (
    id             BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id        BIGINT       NOT NULL,
    name           VARCHAR(100) NOT NULL,
    algorithms     JSON         NOT NULL,
    params         JSON         NOT NULL,
    best_by_metric JSON         NULL,
    created_at     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT fk_comparisons_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE RESTRICT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE INDEX ix_comparisons_user_created ON algorithm_comparisons (user_id, created_at);

CREATE TABLE scheduling_runs (
    id               BIGINT        NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id          BIGINT        NOT NULL,
    comparison_id    BIGINT        NULL,
    algorithm        VARCHAR(16)   NOT NULL,
    params           JSON          NOT NULL,
    use_cluster      TINYINT       NOT NULL DEFAULT 1,
    input_snapshot   JSON          NOT NULL,
    status           VARCHAR(10)   NOT NULL DEFAULT 'RUNNING',
    error_message    VARCHAR(1000) NULL,
    started_at       DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
    completed_at     DATETIME      NULL,
    total_processes  INT           NULL,
    completed_count  INT           NULL,
    rejected_count   INT           NULL,
    avg_waiting      DOUBLE        NULL,
    avg_turnaround   DOUBLE        NULL,
    avg_response     DOUBLE        NULL,
    throughput       DOUBLE        NULL,
    cpu_utilization  DOUBLE        NULL,
    context_switches INT           NULL,
    preemptions      INT           NULL,
    total_time       INT           NULL,
    first_arrival    INT           NULL,
    CONSTRAINT fk_runs_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE RESTRICT,
    CONSTRAINT fk_runs_comparison FOREIGN KEY (comparison_id) REFERENCES algorithm_comparisons (id) ON DELETE SET NULL,
    CONSTRAINT ck_runs_status CHECK (status IN ('RUNNING', 'COMPLETED', 'FAILED')),
    CONSTRAINT ck_runs_algorithm CHECK (algorithm IN ('FCFS', 'SJF', 'PRIORITY_NP', 'HRRN', 'SRTF', 'RR', 'PRIORITY_P', 'MLQ', 'MLFQ'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE INDEX ix_runs_user_started ON scheduling_runs (user_id, started_at);
CREATE INDEX ix_runs_user_algorithm ON scheduling_runs (user_id, algorithm);
CREATE INDEX ix_runs_comparison ON scheduling_runs (comparison_id);

-- Per-process results. job_name/arrival/burst/priority are a SNAPSHOT, so history stays correct if the job is
-- edited or deleted later. process_ref is the id the engine used (equal to the job id at run time).
CREATE TABLE scheduling_results (
    id              BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
    run_id          BIGINT       NOT NULL,
    job_id          BIGINT       NULL,
    process_ref     INT          NOT NULL,
    job_name        VARCHAR(100) NOT NULL,
    arrival_time    INT          NOT NULL,
    burst_time      INT          NOT NULL,
    priority        INT          NOT NULL,
    result_status   VARCHAR(10)  NOT NULL,
    reason          VARCHAR(1000) NULL,
    server_ref      INT          NULL,
    admitted_at     INT          NULL,
    first_start     INT          NULL,
    completion_time INT          NULL,
    turnaround_time INT          NULL,
    waiting_time    INT          NULL,
    response_time   INT          NULL,
    CONSTRAINT fk_results_run FOREIGN KEY (run_id) REFERENCES scheduling_runs (id) ON DELETE CASCADE,
    CONSTRAINT fk_results_job FOREIGN KEY (job_id) REFERENCES jobs (id) ON DELETE SET NULL,
    CONSTRAINT uq_results_run_process UNIQUE (run_id, process_ref),
    CONSTRAINT ck_results_status CHECK (result_status IN ('COMPLETED', 'REJECTED'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE INDEX ix_results_job ON scheduling_results (job_id);

-- The Gantt chart. IDLE segments have no process. Written exactly as the engine produced them.
CREATE TABLE execution_segments (
    id           BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
    run_id       BIGINT       NOT NULL,
    seq          INT          NOT NULL,
    process_ref  INT          NULL,
    job_name     VARCHAR(100) NULL,
    server_ref   INT          NOT NULL DEFAULT 1,
    start_time   INT          NOT NULL,
    end_time     INT          NOT NULL,
    segment_type VARCHAR(8)   NOT NULL,
    end_reason   VARCHAR(20)  NULL,
    queue_level  INT          NULL,
    CONSTRAINT fk_segments_run FOREIGN KEY (run_id) REFERENCES scheduling_runs (id) ON DELETE CASCADE,
    CONSTRAINT uq_segments_run_seq UNIQUE (run_id, seq),
    CONSTRAINT ck_segments_times CHECK (start_time >= 0 AND end_time > start_time),
    CONSTRAINT ck_segments_type CHECK (segment_type IN ('EXEC', 'IDLE'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Reserve/release history. allocated_at/released_at are SIMULATION times (time units), created_at is wall-clock.
CREATE TABLE resource_allocations (
    id           BIGINT   NOT NULL AUTO_INCREMENT PRIMARY KEY,
    run_id       BIGINT   NOT NULL,
    job_id       BIGINT   NULL,
    process_ref  INT      NOT NULL,
    job_name     VARCHAR(100) NULL,
    resource_id  BIGINT   NULL,
    server_ref   INT      NOT NULL,
    cpu_cores    INT      NOT NULL,
    ram_gb       INT      NOT NULL,
    storage_gb   INT      NOT NULL,
    allocated_at INT      NOT NULL,
    released_at  INT      NULL,
    alloc_status VARCHAR(10) NOT NULL,
    created_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT fk_alloc_run FOREIGN KEY (run_id) REFERENCES scheduling_runs (id) ON DELETE CASCADE,
    CONSTRAINT fk_alloc_job FOREIGN KEY (job_id) REFERENCES jobs (id) ON DELETE SET NULL,
    CONSTRAINT fk_alloc_resource FOREIGN KEY (resource_id) REFERENCES resources (id) ON DELETE SET NULL,
    CONSTRAINT ck_alloc_amounts CHECK (cpu_cores > 0 AND ram_gb > 0 AND storage_gb > 0),
    CONSTRAINT ck_alloc_times CHECK (allocated_at >= 0 AND (released_at IS NULL OR released_at >= allocated_at)),
    CONSTRAINT ck_alloc_status CHECK ((alloc_status = 'ALLOCATED' AND released_at IS NULL)
                                      OR (alloc_status = 'RELEASED' AND released_at IS NOT NULL))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE INDEX ix_alloc_run ON resource_allocations (run_id);
CREATE INDEX ix_alloc_job ON resource_allocations (job_id);
CREATE INDEX ix_alloc_resource ON resource_allocations (resource_id);

CREATE TABLE system_events (
    id          BIGINT        NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id     BIGINT        NULL,
    run_id      BIGINT        NULL,
    job_id      BIGINT        NULL,
    event_type  VARCHAR(40)   NOT NULL,
    description VARCHAR(1000) NOT NULL,
    sim_time    INT           NULL,
    created_at  DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT fk_events_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE SET NULL,
    CONSTRAINT fk_events_run FOREIGN KEY (run_id) REFERENCES scheduling_runs (id) ON DELETE SET NULL,
    CONSTRAINT fk_events_job FOREIGN KEY (job_id) REFERENCES jobs (id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE INDEX ix_events_user_created ON system_events (user_id, created_at);
CREATE INDEX ix_events_user_type ON system_events (user_id, event_type);
CREATE INDEX ix_events_run ON system_events (run_id);
CREATE INDEX ix_events_job ON system_events (job_id);

-- Banker's algorithm / deadlock detection checks performed by a user.
CREATE TABLE deadlock_checks (
    id            BIGINT        NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id       BIGINT        NOT NULL,
    check_type    VARCHAR(10)   NOT NULL,
    outcome       VARCHAR(16)   NOT NULL,
    input_data    JSON          NOT NULL,
    result_data   JSON          NOT NULL,
    safe_sequence VARCHAR(255)  NULL,
    explanation   VARCHAR(2000) NULL,
    created_at    DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT fk_deadlock_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE RESTRICT,
    CONSTRAINT ck_deadlock_type CHECK (check_type IN ('SAFETY', 'REQUEST', 'DETECT')),
    CONSTRAINT ck_deadlock_outcome CHECK (outcome IN ('SAFE', 'UNSAFE', 'GRANTED', 'DENIED', 'DEADLOCK', 'NO_DEADLOCK'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
CREATE INDEX ix_deadlock_user_created ON deadlock_checks (user_id, created_at);
