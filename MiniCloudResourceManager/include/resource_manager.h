// resource_manager.h - owns the servers, decides WHERE a job is placed
// (Least-Loaded), reserves/releases resources and keeps the decision log.
//
// It knows nothing about scheduling ORDER; that is scheduler.h's job.
#pragma once

#include <string>
#include <vector>

#include "job.h"
#include "server.h"

// One line of the resource-allocation decision log.
struct LogEntry {
    int time = 0;      // simulation time of the decision
    int jobId = 0;     // 0 = not about a specific job
    std::string message;
};

// How one server was judged for one job.
struct ServerEvaluation {
    int serverId = 0;
    bool eligible = false;
    double score = 0.0;      // load score after placement (only if eligible)
    std::string detail;      // human readable explanation
};

// Result of asking "where could this job go right now?" (no side effects).
struct PlacementDecision {
    bool found = false;
    int serverId = -1;
    std::vector<ServerEvaluation> evaluations;
    std::string summary;     // one-line outcome for the log
    std::string reason;      // when !found: why every server was rejected
};

// "CPU 4, RAM 8 GB, storage 100 GB"
std::string describeResources(const Resources& r);

class ResourceManager {
public:
    explicit ResourceManager(std::vector<Server> servers);

    const std::vector<Server>& servers() const { return servers_; }
    const Server* findServer(int id) const;

    // True if some single server's TOTAL capacity could ever hold the request.
    // If false the job is permanently infeasible and must be REJECTED.
    bool fitsSomeServerTotal(const Resources& request) const;

    // Least-Loaded placement: among servers where every resource fits right
    // now, pick the lowest load score; ties go to the lowest server ID.
    PlacementDecision evaluate(const Job& job) const;

    // Reserve resources on a server for the job. Fails (false) if the job
    // already holds resources or the server cannot fit it.
    bool allocate(Job& job, int serverId);

    // Release the job's resources EXACTLY once. A second call returns false
    // and changes nothing.
    bool release(Job& job);

    // Aggregate utilization across the cluster, average of the three resource
    // types (sum used / sum total for each type).
    double overallUtilization() const;

    // Decision log.
    void log(int time, int jobId, const std::string& message);
    // Writes the per-server evaluation lines and the summary for a decision.
    void logPlacement(int time, const Job& job, const PlacementDecision& decision);
    const std::vector<LogEntry>& logEntries() const { return log_; }

private:
    Server* findServerMutable(int id);

    std::vector<Server> servers_;
    std::vector<LogEntry> log_;
};
