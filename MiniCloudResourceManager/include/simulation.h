// simulation.h - discrete-event simulation that ties Scheduler (order) and
// ResourceManager (placement) together. No console I/O in here.
//
// CLOCK MODEL: time is an integer count of "time units" (tu). The clock only
// jumps to the next EVENT (a job completion or a job arrival). At every event
// time t the simulation does, in this order:
//   1. complete every RUNNING job whose end time <= t and release its resources;
//   2. run one scheduling pass: take all ready jobs (QUEUED/WAITING, arrival <= t),
//      order them with the Scheduler policy, and try to place each one with the
//      ResourceManager. A job that does not fit becomes/stays WAITING and the
//      pass continues with the next job (no head-of-line blocking).
// Jobs on different servers (or on one server with spare capacity) therefore
// run at the same time.
#pragma once

#include <climits>
#include <string>
#include <vector>

#include "job.h"
#include "resource_manager.h"
#include "scheduler.h"

// One finished execution, used to draw the Gantt chart.
struct TimelineEntry {
    int jobId = 0;
    std::string jobName;
    int serverId = 0;
    int arrival = 0;
    int start = 0;
    int end = 0;
};

// Metrics are computed over COMPLETED jobs only (REJECTED / unfinished jobs are
// counted separately and never enter the averages).
//   waiting time    = start time      - arrival time
//   turnaround time = completion time - arrival time
//   duration        = last completion - first arrival (of completed jobs)
//   throughput      = completed jobs / duration   (jobs per time unit)
struct Metrics {
    int totalJobs = 0;
    int completedJobs = 0;
    int rejectedJobs = 0;
    int unfinishedJobs = 0;   // accepted but not COMPLETED yet
    double avgWaitingTime = 0.0;
    double avgTurnaroundTime = 0.0;
    int firstArrival = 0;
    int lastCompletion = 0;
    int simulationDuration = 0;
    double throughput = 0.0;
};

class Simulation {
public:
    Simulation(std::vector<Server> servers, Policy policy);

    // Validates and registers a job. Always returns the new job id; an invalid
    // or permanently infeasible job is stored as REJECTED with a reason.
    int submit(const JobRequest& request);

    // The policy can only change before the first run. Returns success.
    bool setPolicy(Policy policy);
    Policy policy() const { return scheduler_.policy(); }

    // Advance the simulation, processing every event with time <= limit.
    void runUntil(int limit);
    void runToCompletion() { runUntil(INT_MAX); }

    // Completes a RUNNING job at `time` and releases its resources exactly once.
    // Returns false (and changes nothing) if the job is unknown, not RUNNING, or
    // `time` is earlier than start + duration. Repeated calls are harmless.
    bool completeJob(int jobId, int time);

    int clock() const { return clock_; }
    bool hasStarted() const { return started_; }
    const std::vector<Job>& jobs() const { return jobs_; }
    const Job* findJob(int id) const;
    int countInState(JobState state) const;
    const ResourceManager& resources() const { return resources_; }
    const std::vector<LogEntry>& decisionLog() const { return resources_.logEntries(); }
    const std::vector<TimelineEntry>& timeline() const { return timeline_; }
    Metrics metrics() const;

private:
    Job* findJobMutable(int id);
    void processTime(int time);
    void schedulingPass(int time);
    int nextEventTime() const;  // -1 if nothing is left to happen

    ResourceManager resources_;
    Scheduler scheduler_;
    std::vector<Job> jobs_;
    std::vector<TimelineEntry> timeline_;
    int clock_ = 0;
    bool started_ = false;
};
