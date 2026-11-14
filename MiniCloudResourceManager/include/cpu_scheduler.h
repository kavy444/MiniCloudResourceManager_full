// cpu_scheduler.h - CPU scheduling engine (9 algorithms) with optional cluster admission.
//
// TWO LAYERS, KEPT CONCEPTUALLY SEPARATE
//   1. Resource admission (resource_manager.h): decides WHICH SERVER a process is placed on and whether
//      the server has enough free CPU cores / RAM / storage. Only used when SchedConfig::useResources is
//      true ("cluster mode"). Admission is always first-come-first-served by arrival, whatever the CPU
//      algorithm is.
//   2. CPU scheduling (this file): each server has ONE logical CPU. The chosen algorithm decides which
//      admitted (READY) process runs on it, when it is preempted, and for how long.
//   In "cpu mode" (useResources == false) there is one unlimited virtual server, so the result is the
//   classic textbook single-CPU schedule.
//
// TIME: integer "time units" (tu). The clock starts at the earliest arrival and only jumps between
// events (arrival, completion, quantum expiry, aging deadline), so runs are deterministic and cannot loop
// forever.
//
// PRIORITY: a SMALLER number means HIGHER priority (1 is more urgent than 5).
#pragma once

#include <string>
#include <vector>

#include "resource_manager.h"
#include "resources.h"
#include "server.h"

enum class Algorithm { FCFS, SJF, PRIORITY_NP, HRRN, SRTF, RR, PRIORITY_P, MLQ, MLFQ };

std::string algorithmName(Algorithm a);                   // "FCFS", "SJF", "PRIORITY_NP", ...
bool parseAlgorithm(const std::string& text, Algorithm& out);
std::vector<Algorithm> allAlgorithms();

struct SchedProcess {
    int id = 0;
    std::string name;
    int arrival = 0;
    int burst = 0;       // CPU burst, time units
    int priority = 2;    // smaller = higher priority
    int queue = -1;      // MLQ queue index; -1 = derive from priority (see mlqQueueOf)
    Resources demand;    // cores / RAM GB / storage GB (used in cluster mode)
};

// Per-queue policy for Multilevel Queue scheduling. Queue 0 has the highest priority; a process in a higher
// queue always preempts a running process of a lower queue (strict priority between queues).
enum class QueuePolicy { FCFS, RR, SJF, PRIORITY };
std::string queuePolicyName(QueuePolicy p);
bool parseQueuePolicy(const std::string& text, QueuePolicy& out);

struct QueueSpec {
    QueuePolicy policy = QueuePolicy::FCFS;
    int quantum = 0;     // only for RR
};

struct SchedConfig {
    int timeQuantum = 2;                    // Round Robin
    std::vector<QueueSpec> mlq = {          // Multilevel Queue (default 3 queues)
        {QueuePolicy::RR, 2}, {QueuePolicy::RR, 4}, {QueuePolicy::FCFS, 0}};
    std::vector<int> mlfqQuanta = {2, 4, 8};  // MLFQ: quantum per level, 0 = FCFS (run to completion)
    int agingThreshold = 0;                 // MLFQ: promote after waiting this long in a ready queue (0 = off)
    bool useResources = false;              // true = cluster mode (admission through ResourceManager)
    std::vector<Server> servers;            // cluster mode only
};

// ---------- output ----------
struct Segment {
    int processId = -1;          // -1 for IDLE
    int serverId = 1;
    int start = 0;
    int end = 0;
    std::string type;            // "EXEC" or "IDLE"
    std::string endReason;       // EXEC: COMPLETED / PREEMPTED / QUANTUM_EXPIRED ; IDLE: ""
    int queueLevel = -1;         // MLQ/MLFQ queue the process ran from (else -1)
};

struct ProcessResult {
    int id = 0;
    std::string name;
    int arrival = 0, burst = 0, priority = 0;
    int serverId = -1;           // -1 if never placed
    std::string status;          // "COMPLETED" or "REJECTED"
    std::string reason;          // why rejected
    int admittedAt = -1;         // time resources were granted (cluster mode)
    int firstStart = -1;
    int completion = -1;
    int turnaround = -1;         // CT - arrival
    int waiting = -1;            // TAT - burst
    int response = -1;           // first start - arrival
};

struct EngineEvent {
    int time = 0;
    std::string type;            // JOB_ARRIVED, RESOURCES_ALLOCATED, INSUFFICIENT_RESOURCES, JOB_STARTED,
                                 // JOB_PREEMPTED, QUANTUM_EXPIRED, JOB_DEMOTED, JOB_PROMOTED, JOB_COMPLETED,
                                 // RESOURCES_RELEASED, JOB_REJECTED
    int processId = 0;
    int serverId = 0;
    std::string description;
};

struct AllocationRecord {
    int processId = 0;
    int serverId = 0;
    Resources amount;
    int allocatedAt = 0;
    int releasedAt = -1;
};

struct ServerUsage {             // snapshot of one server at one time
    int serverId = 0;
    Resources used;
};
struct ResourceSample {
    int time = 0;
    std::vector<ServerUsage> servers;
};

struct ServerSummary {
    int serverId = 0;
    std::string name;
    Resources total;
    Resources peakUsed;
    double avgCpuUtil = 0, avgRamUtil = 0, avgStorageUtil = 0;  // time-weighted, 0..100 (%)
    double cpuBusyPercent = 0;   // share of total simulated time this server's CPU executed something
    int busyTime = 0;
};

struct ScheduleMetrics {
    int totalProcesses = 0;
    int completed = 0;
    int rejected = 0;
    double avgWaiting = 0, avgTurnaround = 0, avgResponse = 0;
    int totalTime = 0;           // last completion - earliest arrival
    int startTime = 0;           // earliest arrival
    double throughput = 0;       // completed / totalTime  (processes per time unit)
    double cpuUtilization = 0;   // percent: busy CPU time / (servers * totalTime)
    int contextSwitches = 0;
    int preemptions = 0;
};

struct ScheduleResult {
    bool ok = true;
    std::vector<std::string> errors;
    Algorithm algorithm = Algorithm::FCFS;
    std::vector<Segment> segments;           // sorted by server, then start time
    std::vector<ProcessResult> processes;    // sorted by id
    std::vector<EngineEvent> events;         // chronological
    std::vector<AllocationRecord> allocations;
    std::vector<ResourceSample> samples;
    std::vector<ServerSummary> serverSummaries;
    std::vector<LogEntry> decisionLog;       // resource placement explanations (cluster mode)
    ScheduleMetrics metrics;
};

// Returns the list of problems; empty = valid. Checked by runSchedule too.
std::vector<std::string> validateSchedule(const std::vector<SchedProcess>& processes, Algorithm algorithm,
                                          const SchedConfig& config);

// MLQ queue of a process: its explicit queue, else clamp(priority - 1, 0, numQueues - 1).
int mlqQueueOf(const SchedProcess& p, int numQueues);

ScheduleResult runSchedule(const std::vector<SchedProcess>& processes, Algorithm algorithm,
                           const SchedConfig& config);
