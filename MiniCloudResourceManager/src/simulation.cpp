#include "simulation.h"

#include <algorithm>
#include <utility>

Simulation::Simulation(std::vector<Server> servers, Policy policy)
    : resources_(std::move(servers)), scheduler_(policy) {}

const Job* Simulation::findJob(int id) const {
    if (id < 1 || id > static_cast<int>(jobs_.size())) return nullptr;
    return &jobs_[static_cast<std::size_t>(id - 1)];
}

Job* Simulation::findJobMutable(int id) {
    if (id < 1 || id > static_cast<int>(jobs_.size())) return nullptr;
    return &jobs_[static_cast<std::size_t>(id - 1)];
}

int Simulation::countInState(JobState state) const {
    return static_cast<int>(std::count_if(jobs_.begin(), jobs_.end(),
                                          [state](const Job& j) { return j.state == state; }));
}

bool Simulation::setPolicy(Policy policy) {
    if (started_) return false;
    scheduler_.setPolicy(policy);
    return true;
}

int Simulation::submit(const JobRequest& request) {
    Job job;
    job.id = static_cast<int>(jobs_.size()) + 1;  // ids = submission order
    job.name = request.name;
    job.demand = request.demand;
    job.duration = request.duration;
    job.priority = request.priority;
    job.arrivalTime = request.arrivalTime;

    std::string error = validateJobRequest(request);
    if (error.empty() && !resources_.fitsSomeServerTotal(request.demand)) {
        error = "request (" + describeResources(request.demand) +
                ") exceeds the total capacity of every server, so it can never run";
    }

    const std::string label = "Job " + std::to_string(job.id) + " '" + job.name + "'";
    if (!error.empty()) {
        job.transitionTo(JobState::Rejected);  // QUEUED -> REJECTED
        job.reason = "Rejected: " + error;
        resources_.log(clock_, job.id, label + " REJECTED: " + error);
    } else {
        resources_.log(clock_, job.id,
                       label + " submitted (" + describeResources(job.demand) + ", duration " +
                           std::to_string(job.duration) + " tu, priority " + toString(job.priority) +
                           ", arrival t=" + std::to_string(job.arrivalTime) + ") -> QUEUED");
    }
    jobs_.push_back(job);
    return job.id;
}

bool Simulation::completeJob(int jobId, int time) {
    Job* job = findJobMutable(jobId);
    if (job == nullptr) return false;
    if (job->state != JobState::Running) {
        resources_.log(time, jobId, "Ignored completion request for job " + std::to_string(jobId) +
                                        ": job is " + toString(job->state) + ", not RUNNING");
        return false;
    }
    if (time < job->startTime + job->duration) return false;  // not finished yet
    if (!resources_.release(*job)) return false;               // exactly-once release

    job->transitionTo(JobState::Completed);
    job->completionTime = time;
    timeline_.push_back(TimelineEntry{job->id, job->name, job->serverId, job->arrivalTime, job->startTime, time});

    const Server* server = resources_.findServer(job->serverId);
    resources_.log(time, job->id,
                   "Job " + std::to_string(job->id) + " '" + job->name + "' COMPLETED; released " +
                       describeResources(job->demand) + " on " + server->name() + " (free now: " +
                       describeResources(server->available()) + ")");
    return true;
}

void Simulation::schedulingPass(int time) {
    std::vector<Job*> ready;
    for (Job& job : jobs_) {
        const bool pending = job.state == JobState::Queued || job.state == JobState::Waiting;
        if (pending && job.arrivalTime <= time) ready.push_back(&job);
    }
    scheduler_.sortReadyQueue(ready);  // CPU-scheduling ORDER

    for (Job* job : ready) {
        const PlacementDecision decision = resources_.evaluate(*job);  // server PLACEMENT
        if (decision.found) {
            const bool wasWaiting = job->state == JobState::Waiting;
            resources_.logPlacement(time, *job, decision);
            if (!resources_.allocate(*job, decision.serverId)) {
                resources_.log(time, job->id, "Internal error: allocation failed unexpectedly");
                continue;
            }
            job->transitionTo(JobState::Running);
            job->startTime = time;
            job->reason.clear();
            const Server* server = resources_.findServer(decision.serverId);
            resources_.log(time, job->id,
                           "Job " + std::to_string(job->id) + " '" + job->name + "' RUNNING on " + server->name() +
                               " until t=" + std::to_string(time + job->duration) + " (waited " +
                               std::to_string(time - job->arrivalTime) + " tu" +
                               (wasWaiting ? ", started on retry after resources were released" : "") + ")");
        } else {
            if (job->state == JobState::Queued) {
                job->transitionTo(JobState::Waiting);
                resources_.logPlacement(time, *job, decision);  // log once, on first failure
            }
            job->reason = decision.reason;  // refreshed on every retry
        }
    }
}

int Simulation::nextEventTime() const {
    int next = -1;
    for (const Job& job : jobs_) {
        int candidate = -1;
        if (job.state == JobState::Running) {
            candidate = job.startTime + job.duration;
        } else if ((job.state == JobState::Queued || job.state == JobState::Waiting) && job.arrivalTime > clock_) {
            candidate = job.arrivalTime;
        }
        if (candidate >= 0 && (next < 0 || candidate < next)) next = candidate;
    }
    return next;
}

void Simulation::processTime(int time) {
    for (Job& job : jobs_) {
        if (job.state == JobState::Running && job.startTime + job.duration <= time) {
            completeJob(job.id, job.startTime + job.duration);
        }
    }
    schedulingPass(time);  // runs after releases, so waiting jobs are retried
}

void Simulation::runUntil(int limit) {
    started_ = true;
    while (true) {
        processTime(clock_);
        const int next = nextEventTime();
        if (next < 0 || next > limit || next <= clock_) {  // next <= clock_ is a safety stop
            if (limit != INT_MAX && limit > clock_) clock_ = limit;
            break;
        }
        clock_ = next;
    }
}

Metrics Simulation::metrics() const {
    Metrics m;
    m.totalJobs = static_cast<int>(jobs_.size());
    long totalWaiting = 0, totalTurnaround = 0;
    bool any = false;
    for (const Job& job : jobs_) {
        if (job.state == JobState::Rejected) {
            ++m.rejectedJobs;
        } else if (job.state != JobState::Completed) {
            ++m.unfinishedJobs;
        } else {
            ++m.completedJobs;
            totalWaiting += job.waitingTime();
            totalTurnaround += job.turnaroundTime();
            if (!any || job.arrivalTime < m.firstArrival) m.firstArrival = job.arrivalTime;
            if (!any || job.completionTime > m.lastCompletion) m.lastCompletion = job.completionTime;
            any = true;
        }
    }
    if (m.completedJobs > 0) {
        m.avgWaitingTime = static_cast<double>(totalWaiting) / m.completedJobs;
        m.avgTurnaroundTime = static_cast<double>(totalTurnaround) / m.completedJobs;
        m.simulationDuration = m.lastCompletion - m.firstArrival;
        if (m.simulationDuration > 0) m.throughput = static_cast<double>(m.completedJobs) / m.simulationDuration;
    }
    return m;
}
