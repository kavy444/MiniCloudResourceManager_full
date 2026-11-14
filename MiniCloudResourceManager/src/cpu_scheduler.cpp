#include "cpu_scheduler.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>
#include <set>

// -------------------------------------------------------------------------------------------------
// names and parsing
// -------------------------------------------------------------------------------------------------
std::string algorithmName(Algorithm a) {
    switch (a) {
        case Algorithm::FCFS: return "FCFS";
        case Algorithm::SJF: return "SJF";
        case Algorithm::PRIORITY_NP: return "PRIORITY_NP";
        case Algorithm::HRRN: return "HRRN";
        case Algorithm::SRTF: return "SRTF";
        case Algorithm::RR: return "RR";
        case Algorithm::PRIORITY_P: return "PRIORITY_P";
        case Algorithm::MLQ: return "MLQ";
        case Algorithm::MLFQ: return "MLFQ";
    }
    return "FCFS";
}

std::vector<Algorithm> allAlgorithms() {
    return {Algorithm::FCFS, Algorithm::SJF, Algorithm::PRIORITY_NP, Algorithm::HRRN, Algorithm::SRTF,
            Algorithm::RR,   Algorithm::PRIORITY_P, Algorithm::MLQ, Algorithm::MLFQ};
}

bool parseAlgorithm(const std::string& text, Algorithm& out) {
    for (Algorithm a : allAlgorithms()) {
        if (algorithmName(a) == text) {
            out = a;
            return true;
        }
    }
    return false;
}

std::string queuePolicyName(QueuePolicy p) {
    switch (p) {
        case QueuePolicy::FCFS: return "FCFS";
        case QueuePolicy::RR: return "RR";
        case QueuePolicy::SJF: return "SJF";
        case QueuePolicy::PRIORITY: return "PRIORITY";
    }
    return "FCFS";
}

bool parseQueuePolicy(const std::string& text, QueuePolicy& out) {
    for (QueuePolicy p : {QueuePolicy::FCFS, QueuePolicy::RR, QueuePolicy::SJF, QueuePolicy::PRIORITY}) {
        if (queuePolicyName(p) == text) {
            out = p;
            return true;
        }
    }
    return false;
}

int mlqQueueOf(const SchedProcess& p, int numQueues) {
    if (numQueues < 1) return 0;
    if (p.queue >= 0) return std::min(p.queue, numQueues - 1);
    return std::max(0, std::min(p.priority - 1, numQueues - 1));
}

// -------------------------------------------------------------------------------------------------
// validation
// -------------------------------------------------------------------------------------------------
namespace {
constexpr int kMaxProcesses = 1000;
constexpr int kMaxBurst = 100000;
constexpr int kMaxTime = 1000000;
constexpr int kMaxDemand = 1000000;
constexpr long kStepLimit = 5000000;
}  // namespace

std::vector<std::string> validateSchedule(const std::vector<SchedProcess>& processes, Algorithm algorithm,
                                          const SchedConfig& config) {
    std::vector<std::string> errors;
    if (processes.empty()) errors.push_back("at least one process is required");
    if (static_cast<int>(processes.size()) > kMaxProcesses) {
        errors.push_back("too many processes (maximum " + std::to_string(kMaxProcesses) + ")");
    }
    std::set<int> seen;
    for (const SchedProcess& p : processes) {
        const std::string who = "process " + std::to_string(p.id) + ": ";
        if (p.id < 1) errors.push_back(who + "id must be at least 1");
        if (!seen.insert(p.id).second) errors.push_back(who + "duplicate id");
        if (p.name.size() > 100) errors.push_back(who + "name longer than 100 characters");
        if (p.arrival < 0 || p.arrival > kMaxTime) {
            errors.push_back(who + "arrival time must be between 0 and " + std::to_string(kMaxTime));
        }
        if (p.burst < 1 || p.burst > kMaxBurst) {
            errors.push_back(who + "burst time must be between 1 and " + std::to_string(kMaxBurst));
        }
        if (p.priority < 0 || p.priority > 1000) errors.push_back(who + "priority must be between 0 and 1000");
        if (p.demand.cpu < 1 || p.demand.ramGb < 1 || p.demand.storageGb < 1 || p.demand.cpu > kMaxDemand ||
            p.demand.ramGb > kMaxDemand || p.demand.storageGb > kMaxDemand) {
            errors.push_back(who + "cpu, ram and storage must each be between 1 and " + std::to_string(kMaxDemand));
        }
        if (algorithm == Algorithm::MLQ && p.queue >= static_cast<int>(config.mlq.size())) {
            errors.push_back(who + "queue index " + std::to_string(p.queue) + " does not exist");
        }
        if (p.queue < -1) errors.push_back(who + "queue must be -1 (automatic) or a queue index");
    }
    if (algorithm == Algorithm::RR && (config.timeQuantum < 1 || config.timeQuantum > kMaxBurst)) {
        errors.push_back("time quantum must be between 1 and " + std::to_string(kMaxBurst));
    }
    if (algorithm == Algorithm::MLQ) {
        if (config.mlq.empty() || config.mlq.size() > 8) errors.push_back("MLQ needs between 1 and 8 queues");
        for (std::size_t i = 0; i < config.mlq.size(); ++i) {
            if (config.mlq[i].policy == QueuePolicy::RR && (config.mlq[i].quantum < 1 || config.mlq[i].quantum > kMaxBurst)) {
                errors.push_back("MLQ queue " + std::to_string(i) + ": round-robin quantum must be at least 1");
            }
        }
    }
    if (algorithm == Algorithm::MLFQ) {
        if (config.mlfqQuanta.empty() || config.mlfqQuanta.size() > 8) errors.push_back("MLFQ needs between 1 and 8 queues");
        for (std::size_t i = 0; i < config.mlfqQuanta.size(); ++i) {
            if (config.mlfqQuanta[i] < 0 || config.mlfqQuanta[i] > kMaxBurst) {
                errors.push_back("MLFQ queue " + std::to_string(i) + ": quantum must be 0 (FCFS) or 1.." + std::to_string(kMaxBurst));
            }
        }
        if (config.agingThreshold < 0 || config.agingThreshold > kMaxTime) errors.push_back("aging threshold must be 0 or positive");
    }
    if (config.useResources) {
        if (config.servers.empty()) errors.push_back("cluster mode needs at least one server");
        if (config.servers.size() > 16) errors.push_back("too many servers (maximum 16)");
    }
    return errors;
}

// -------------------------------------------------------------------------------------------------
// the engine
// -------------------------------------------------------------------------------------------------
namespace {

enum class PState { NotArrived, WaitingRes, Ready, Running, Done, Rejected };

struct PS {  // per-process simulation state
    SchedProcess p;
    PState st = PState::NotArrived;
    int remaining = 0;
    int server = -1;          // index into servers
    int firstStart = -1;
    int completion = -1;
    long seq = 0;             // order of entering a ready queue (smaller = earlier)
    int level = 0;            // MLFQ level
    int quantumLeft = -1;     // -1 = no quantum
    int waitingSince = 0;     // when the process last started waiting in a ready queue
    int admittedAt = -1;
    int allocIndex = -1;
    Job res;                  // used for ResourceManager calls (cluster mode)
    std::string reason;
};

struct SV {  // per-server (one logical CPU) state
    int id = 1;
    std::string name;
    Resources total;
    std::vector<int> ready;
    int running = -1;
    int segStart = 0;
    long busy = 0;
    Resources peak;
    double integral[3] = {0, 0, 0};
    std::vector<Segment> segs;
};

class Engine {
public:
    Engine(const std::vector<SchedProcess>& processes, Algorithm alg, const SchedConfig& cfg)
        : alg_(alg), cfg_(cfg) {
        std::vector<SchedProcess> sorted = processes;
        std::sort(sorted.begin(), sorted.end(), [](const SchedProcess& a, const SchedProcess& b) {
            return a.id < b.id;
        });
        for (const SchedProcess& p : sorted) {
            PS x;
            x.p = p;
            x.remaining = p.burst;
            x.res.id = p.id;
            x.res.name = p.name;
            x.res.demand = p.demand;
            ps_.push_back(x);
        }
        if (cfg_.useResources) {
            rm_ = std::make_unique<ResourceManager>(cfg_.servers);
            for (const Server& s : cfg_.servers) {
                SV v;
                v.id = s.id();
                v.name = s.name();
                v.total = s.total();
                sv_.push_back(v);
            }
        } else {
            SV v;
            v.id = 1;
            v.name = "CPU";
            sv_.push_back(v);
        }
    }

    ScheduleResult run() {
        ScheduleResult result;
        result.algorithm = alg_;
        int t = std::numeric_limits<int>::max();
        for (const PS& x : ps_) t = std::min(t, x.p.arrival);
        const int startTime = t;
        long steps = 0;
        std::vector<int> expired;  // processes whose quantum ended at the current time

        while (true) {
            if (++steps > kStepLimit) {
                result.ok = false;
                result.errors.push_back("simulation exceeded the step limit; reduce burst times or increase the quantum");
                return result;
            }
            admitPending(t);
            requeueExpired(expired, t);
            ageProcesses(t);
            for (std::size_t s = 0; s < sv_.size(); ++s) dispatch(s, t);
            takeSample(t);

            const int next = nextEventTime(t);
            if (next == std::numeric_limits<int>::max()) break;

            const int dt = next - t;
            for (std::size_t s = 0; s < sv_.size(); ++s) advanceServer(s, dt);
            t = next;
            for (std::size_t s = 0; s < sv_.size(); ++s) finishOrExpire(s, t, expired);
        }

        // Safety net: anything still waiting for resources can never be placed.
        for (PS& x : ps_) {
            if (x.st == PState::WaitingRes || x.st == PState::NotArrived) {
                x.st = PState::Rejected;
                x.reason = "never obtained resources: " + x.reason;
                emit(t, "JOB_REJECTED", x.p.id, 0, "Process " + std::to_string(x.p.id) + " rejected: " + x.reason);
            }
        }
        return finish(startTime, t);
    }

private:
    // ---- helpers ----
    void emit(int time, const std::string& type, int pid, int serverId, const std::string& description) {
        EngineEvent e;
        e.time = time;
        e.type = type;
        e.processId = pid;
        e.serverId = serverId;
        e.description = description;
        events_.push_back(e);
    }

    std::string label(const PS& x) const { return "P" + std::to_string(x.p.id) + " '" + x.p.name + "'"; }

    bool queueBased() const { return alg_ == Algorithm::MLQ || alg_ == Algorithm::MLFQ; }

    int levelOf(const PS& x) const {
        if (alg_ == Algorithm::MLQ) return mlqQueueOf(x.p, static_cast<int>(cfg_.mlq.size()));
        if (alg_ == Algorithm::MLFQ) return x.level;
        return -1;
    }

    // Quantum for the process at its current queue; -1 means "no quantum, run until done / preempted".
    int quantumFor(const PS& x) const {
        switch (alg_) {
            case Algorithm::RR: return cfg_.timeQuantum;
            case Algorithm::MLQ: {
                const QueueSpec& spec = cfg_.mlq[static_cast<std::size_t>(levelOf(x))];
                return spec.policy == QueuePolicy::RR ? spec.quantum : -1;
            }
            case Algorithm::MLFQ: {
                const int q = cfg_.mlfqQuanta[static_cast<std::size_t>(x.level)];
                return q > 0 ? q : -1;
            }
            default: return -1;
        }
    }

    // Smaller value = more urgent; used to decide whether a ready process preempts the running one.
    long primaryKey(const PS& x) const {
        if (alg_ == Algorithm::SRTF) return x.remaining;
        if (alg_ == Algorithm::PRIORITY_P) return x.p.priority;
        return 0;
    }

    // True if `a` should run before `b` (deterministic total order; ids are unique).
    bool before(const PS& a, const PS& b, int t) const {
        auto tieBreak = [&](const PS& x, const PS& y) {
            if (x.seq != y.seq) return x.seq < y.seq;
            if (x.p.arrival != y.p.arrival) return x.p.arrival < y.p.arrival;
            return x.p.id < y.p.id;
        };
        auto byArrival = [&](const PS& x, const PS& y) {
            if (x.p.arrival != y.p.arrival) return x.p.arrival < y.p.arrival;
            return x.p.id < y.p.id;
        };
        switch (alg_) {
            case Algorithm::FCFS:
            case Algorithm::RR:
            case Algorithm::MLFQ:
                return tieBreak(a, b);
            case Algorithm::SJF:
                if (a.p.burst != b.p.burst) return a.p.burst < b.p.burst;
                return byArrival(a, b);
            case Algorithm::PRIORITY_NP:
            case Algorithm::PRIORITY_P:
                if (a.p.priority != b.p.priority) return a.p.priority < b.p.priority;
                return tieBreak(a, b);
            case Algorithm::SRTF:
                if (a.remaining != b.remaining) return a.remaining < b.remaining;
                return tieBreak(a, b);
            case Algorithm::HRRN: {
                // response ratio = (waiting + burst) / burst ; compare by cross-multiplication (no floats)
                const long long ra = static_cast<long long>(t - a.p.arrival + a.p.burst) * b.p.burst;
                const long long rb = static_cast<long long>(t - b.p.arrival + b.p.burst) * a.p.burst;
                if (ra != rb) return ra > rb;
                return byArrival(a, b);
            }
            case Algorithm::MLQ: {
                const QueueSpec& spec = cfg_.mlq[static_cast<std::size_t>(levelOf(a))];  // same level as b
                if (spec.policy == QueuePolicy::SJF) {
                    if (a.p.burst != b.p.burst) return a.p.burst < b.p.burst;
                    return byArrival(a, b);
                }
                if (spec.policy == QueuePolicy::PRIORITY) {
                    if (a.p.priority != b.p.priority) return a.p.priority < b.p.priority;
                    return tieBreak(a, b);
                }
                return tieBreak(a, b);  // FCFS and RR queues are FIFO
            }
        }
        return a.p.id < b.p.id;
    }

    int best(const std::vector<int>& candidates, int t) const {
        int bestIdx = -1;
        for (int i : candidates) {
            if (bestIdx < 0 || before(ps_[static_cast<std::size_t>(i)], ps_[static_cast<std::size_t>(bestIdx)], t)) bestIdx = i;
        }
        return bestIdx;
    }

    // ---- segments ----
    void openSegment(SV& v, int pi, int t) {
        const PS& x = ps_[static_cast<std::size_t>(pi)];
        const int level = levelOf(x);
        v.segStart = t;
        // Merge with the previous segment when the same process simply continues (no real switch).
        if (!v.segs.empty()) {
            const Segment& last = v.segs.back();
            if (last.type == "EXEC" && last.processId == x.p.id && last.end == t && last.queueLevel == level) {
                v.segStart = last.start;
                v.segs.pop_back();
            }
        }
    }

    void closeSegment(SV& v, int t, const std::string& reason) {
        const PS& x = ps_[static_cast<std::size_t>(v.running)];
        if (t <= v.segStart) return;
        Segment s;
        s.processId = x.p.id;
        s.serverId = v.id;
        s.start = v.segStart;
        s.end = t;
        s.type = "EXEC";
        s.endReason = reason;
        s.queueLevel = levelOf(x);
        v.segs.push_back(s);
    }

    // ---- admission (resource layer) ----
    void admit(int pi, std::size_t svIdx, int t) {
        PS& x = ps_[static_cast<std::size_t>(pi)];
        SV& v = sv_[svIdx];
        x.st = PState::Ready;
        x.server = static_cast<int>(svIdx);
        x.admittedAt = t;
        x.seq = ++seqCounter_;
        x.waitingSince = t;
        x.level = 0;
        x.reason.clear();
        v.ready.push_back(pi);
        if (cfg_.useResources) {
            AllocationRecord rec;
            rec.processId = x.p.id;
            rec.serverId = v.id;
            rec.amount = x.p.demand;
            rec.allocatedAt = t;
            x.allocIndex = static_cast<int>(allocations_.size());
            allocations_.push_back(rec);
            emit(t, "RESOURCES_ALLOCATED", x.p.id, v.id,
                 label(x) + " allocated " + describeResources(x.p.demand) + " on " + v.name);
        }
    }

    void admitPending(int t) {
        std::vector<int> pending;
        for (std::size_t i = 0; i < ps_.size(); ++i) {
            const PS& x = ps_[i];
            if ((x.st == PState::NotArrived && x.p.arrival <= t) || x.st == PState::WaitingRes) {
                pending.push_back(static_cast<int>(i));
            }
        }
        // Admission order is FCFS by arrival (then id), independent of the CPU algorithm.
        std::sort(pending.begin(), pending.end(), [this](int a, int b) {
            const PS& x = ps_[static_cast<std::size_t>(a)];
            const PS& y = ps_[static_cast<std::size_t>(b)];
            if (x.p.arrival != y.p.arrival) return x.p.arrival < y.p.arrival;
            return x.p.id < y.p.id;
        });
        for (int pi : pending) {
            PS& x = ps_[static_cast<std::size_t>(pi)];
            const bool firstSeen = x.st == PState::NotArrived;
            if (firstSeen) emit(x.p.arrival, "JOB_ARRIVED", x.p.id, 0, label(x) + " arrived (burst " + std::to_string(x.p.burst) + ")");
            if (!cfg_.useResources) {
                admit(pi, 0, t);
                continue;
            }
            if (!rm_->fitsSomeServerTotal(x.p.demand)) {
                x.st = PState::Rejected;
                x.reason = "request (" + describeResources(x.p.demand) + ") exceeds the total capacity of every server";
                rm_->log(t, x.p.id, "Job " + std::to_string(x.p.id) + " REJECTED: " + x.reason);
                emit(t, "JOB_REJECTED", x.p.id, 0, label(x) + " rejected: " + x.reason);
                continue;
            }
            const PlacementDecision d = rm_->evaluate(x.res);
            if (d.found) {
                rm_->logPlacement(t, x.res, d);
                rm_->allocate(x.res, d.serverId);
                std::size_t idx = 0;
                for (std::size_t s = 0; s < sv_.size(); ++s) if (sv_[s].id == d.serverId) idx = s;
                admit(pi, idx, t);
            } else {
                if (firstSeen) {
                    rm_->logPlacement(t, x.res, d);
                    emit(t, "INSUFFICIENT_RESOURCES", x.p.id, 0, label(x) + " must wait: " + d.reason);
                }
                x.st = PState::WaitingRes;
                x.reason = d.reason;
            }
        }
    }

    // ---- CPU layer ----
    void requeueExpired(std::vector<int>& expired, int t) {
        for (int pi : expired) {
            PS& x = ps_[static_cast<std::size_t>(pi)];
            SV& v = sv_[static_cast<std::size_t>(x.server)];
            x.st = PState::Ready;
            x.seq = ++seqCounter_;  // tail of its queue (after arrivals that happened at the same instant)
            x.waitingSince = t;
            if (alg_ == Algorithm::MLFQ && x.level + 1 < static_cast<int>(cfg_.mlfqQuanta.size())) {
                ++x.level;
                emit(t, "JOB_DEMOTED", x.p.id, v.id, label(x) + " used its full quantum and moves to queue " + std::to_string(x.level));
            }
            v.ready.push_back(pi);
        }
        expired.clear();
    }

    void ageProcesses(int t) {
        if (alg_ != Algorithm::MLFQ || cfg_.agingThreshold <= 0) return;
        for (SV& v : sv_) {
            for (int pi : v.ready) {
                PS& x = ps_[static_cast<std::size_t>(pi)];
                if (x.level > 0 && t - x.waitingSince >= cfg_.agingThreshold) {
                    --x.level;
                    x.seq = ++seqCounter_;
                    x.waitingSince = t;
                    emit(t, "JOB_PROMOTED", x.p.id, v.id, label(x) + " waited " + std::to_string(cfg_.agingThreshold) +
                         " tu and is promoted to queue " + std::to_string(x.level));
                }
            }
        }
    }

    void startProcess(SV& v, int pi, int t) {
        PS& x = ps_[static_cast<std::size_t>(pi)];
        v.ready.erase(std::find(v.ready.begin(), v.ready.end(), pi));
        v.running = pi;
        x.st = PState::Running;
        x.quantumLeft = quantumFor(x);
        if (x.firstStart < 0) {
            x.firstStart = t;
            emit(t, "JOB_STARTED", x.p.id, v.id, label(x) + " starts executing on " + v.name);
        }
        openSegment(v, pi, t);
    }

    void preemptRunning(SV& v, int t, int byPid) {
        PS& r = ps_[static_cast<std::size_t>(v.running)];
        closeSegment(v, t, "PREEMPTED");
        r.st = PState::Ready;
        if (queueBased()) r.seq = ++seqCounter_;  // back of its queue
        r.waitingSince = t;
        v.ready.push_back(v.running);
        ++preemptions_;
        emit(t, "JOB_PREEMPTED", r.p.id, v.id, label(r) + " preempted by P" + std::to_string(byPid) + " (" +
             std::to_string(r.remaining) + " tu left)");
        v.running = -1;
    }

    void dispatch(std::size_t s, int t) {
        SV& v = sv_[s];
        if (v.ready.empty()) return;
        switch (alg_) {
            case Algorithm::FCFS:
            case Algorithm::SJF:
            case Algorithm::PRIORITY_NP:
            case Algorithm::HRRN:
            case Algorithm::RR:
                if (v.running < 0) startProcess(v, best(v.ready, t), t);
                break;
            case Algorithm::SRTF:
            case Algorithm::PRIORITY_P: {
                const int cand = best(v.ready, t);
                if (v.running < 0) {
                    startProcess(v, cand, t);
                } else if (primaryKey(ps_[static_cast<std::size_t>(cand)]) < primaryKey(ps_[static_cast<std::size_t>(v.running)])) {
                    // strictly better only: ties keep the running process (no pointless switches)
                    preemptRunning(v, t, ps_[static_cast<std::size_t>(cand)].p.id);
                    startProcess(v, cand, t);
                }
                break;
            }
            case Algorithm::MLQ:
            case Algorithm::MLFQ: {
                int minLevel = std::numeric_limits<int>::max();
                for (int pi : v.ready) minLevel = std::min(minLevel, levelOf(ps_[static_cast<std::size_t>(pi)]));
                std::vector<int> candidates;
                for (int pi : v.ready) if (levelOf(ps_[static_cast<std::size_t>(pi)]) == minLevel) candidates.push_back(pi);
                const int cand = best(candidates, t);
                if (v.running < 0) {
                    startProcess(v, cand, t);
                } else if (minLevel < levelOf(ps_[static_cast<std::size_t>(v.running)])) {
                    preemptRunning(v, t, ps_[static_cast<std::size_t>(cand)].p.id);
                    startProcess(v, cand, t);
                }
                break;
            }
        }
    }

    int nextEventTime(int t) const {
        int next = std::numeric_limits<int>::max();
        for (const PS& x : ps_) {
            if (x.st == PState::NotArrived && x.p.arrival > t) next = std::min(next, x.p.arrival);
        }
        for (const SV& v : sv_) {
            if (v.running >= 0) {
                const PS& r = ps_[static_cast<std::size_t>(v.running)];
                int span = r.remaining;
                if (r.quantumLeft >= 0) span = std::min(span, r.quantumLeft);
                next = std::min(next, t + span);
            }
            if (alg_ == Algorithm::MLFQ && cfg_.agingThreshold > 0) {
                for (int pi : v.ready) {
                    const PS& x = ps_[static_cast<std::size_t>(pi)];
                    if (x.level > 0) {
                        const int deadline = x.waitingSince + cfg_.agingThreshold;
                        if (deadline > t) next = std::min(next, deadline);
                    }
                }
            }
        }
        return next;
    }

    void advanceServer(std::size_t s, int dt) {
        SV& v = sv_[s];
        if (cfg_.useResources) {
            const Server* server = rm_->findServer(v.id);
            const Resources u = server->used();
            v.integral[0] += static_cast<double>(u.cpu) * dt;
            v.integral[1] += static_cast<double>(u.ramGb) * dt;
            v.integral[2] += static_cast<double>(u.storageGb) * dt;
        }
        if (v.running < 0) return;
        PS& r = ps_[static_cast<std::size_t>(v.running)];
        r.remaining -= dt;
        if (r.quantumLeft >= 0) r.quantumLeft -= dt;
        v.busy += dt;
    }

    void finishOrExpire(std::size_t s, int t, std::vector<int>& expired) {
        SV& v = sv_[s];
        if (v.running < 0) return;
        const int pi = v.running;
        PS& x = ps_[static_cast<std::size_t>(pi)];
        if (x.remaining == 0) {
            closeSegment(v, t, "COMPLETED");
            x.st = PState::Done;
            x.completion = t;
            v.running = -1;
            emit(t, "JOB_COMPLETED", x.p.id, v.id, label(x) + " completed on " + v.name);
            if (cfg_.useResources) {
                rm_->release(x.res);
                allocations_[static_cast<std::size_t>(x.allocIndex)].releasedAt = t;
                emit(t, "RESOURCES_RELEASED", x.p.id, v.id, label(x) + " released " + describeResources(x.p.demand) + " on " + v.name);
                rm_->log(t, x.p.id, "Job " + std::to_string(x.p.id) + " '" + x.p.name + "' COMPLETED; released " +
                         describeResources(x.p.demand) + " on " + v.name);
            }
        } else if (x.quantumLeft == 0) {
            closeSegment(v, t, "QUANTUM_EXPIRED");
            v.running = -1;
            ++preemptions_;
            emit(t, "QUANTUM_EXPIRED", x.p.id, v.id, label(x) + " quantum expired (" + std::to_string(x.remaining) + " tu left)");
            expired.push_back(pi);
        }
    }

    void takeSample(int t) {
        if (!cfg_.useResources) return;
        ResourceSample sample;
        sample.time = t;
        for (SV& v : sv_) {
            const Resources u = rm_->findServer(v.id)->used();
            sample.servers.push_back(ServerUsage{v.id, u});
            v.peak.cpu = std::max(v.peak.cpu, u.cpu);
            v.peak.ramGb = std::max(v.peak.ramGb, u.ramGb);
            v.peak.storageGb = std::max(v.peak.storageGb, u.storageGb);
        }
        samples_.push_back(sample);
    }

    // ---- build the result ----
    ScheduleResult finish(int startTime, int endClock) {
        (void)endClock;
        ScheduleResult r;
        r.algorithm = alg_;
        ScheduleMetrics& m = r.metrics;
        m.totalProcesses = static_cast<int>(ps_.size());

        int lastCompletion = startTime;
        long sumWait = 0, sumTat = 0, sumResp = 0;
        for (const PS& x : ps_) {
            ProcessResult pr;
            pr.id = x.p.id;
            pr.name = x.p.name;
            pr.arrival = x.p.arrival;
            pr.burst = x.p.burst;
            pr.priority = x.p.priority;
            pr.admittedAt = x.admittedAt;
            if (x.server >= 0) pr.serverId = sv_[static_cast<std::size_t>(x.server)].id;
            if (x.st == PState::Done) {
                pr.status = "COMPLETED";
                pr.firstStart = x.firstStart;
                pr.completion = x.completion;
                pr.turnaround = x.completion - x.p.arrival;
                pr.waiting = pr.turnaround - x.p.burst;
                pr.response = x.firstStart - x.p.arrival;
                ++m.completed;
                sumWait += pr.waiting;
                sumTat += pr.turnaround;
                sumResp += pr.response;
                lastCompletion = std::max(lastCompletion, x.completion);
            } else {
                pr.status = "REJECTED";
                pr.reason = x.reason;
                ++m.rejected;
            }
            r.processes.push_back(pr);
        }

        m.startTime = startTime;
        if (m.completed > 0) {
            m.avgWaiting = static_cast<double>(sumWait) / m.completed;
            m.avgTurnaround = static_cast<double>(sumTat) / m.completed;
            m.avgResponse = static_cast<double>(sumResp) / m.completed;
            m.totalTime = lastCompletion - startTime;
            if (m.totalTime > 0) m.throughput = static_cast<double>(m.completed) / m.totalTime;
        }

        // Fill IDLE segments so every server's timeline covers [startTime, lastCompletion].
        long totalBusy = 0;
        for (SV& v : sv_) {
            std::sort(v.segs.begin(), v.segs.end(), [](const Segment& a, const Segment& b) { return a.start < b.start; });
            std::vector<Segment> full;
            int cursor = startTime;
            auto addIdle = [&](int from, int to) {
                if (to <= from) return;
                Segment idle;
                idle.processId = -1;
                idle.serverId = v.id;
                idle.start = from;
                idle.end = to;
                idle.type = "IDLE";
                full.push_back(idle);
            };
            int prevPid = -1;
            for (const Segment& s : v.segs) {
                addIdle(cursor, s.start);
                full.push_back(s);
                cursor = s.end;
                if (prevPid != -1 && prevPid != s.processId) ++m.contextSwitches;  // see README: switch convention
                prevPid = s.processId;
            }
            addIdle(cursor, lastCompletion);
            for (const Segment& s : full) r.segments.push_back(s);
            totalBusy += v.busy;

            ServerSummary ss;
            ss.serverId = v.id;
            ss.name = v.name;
            ss.total = v.total;
            ss.peakUsed = v.peak;
            ss.busyTime = static_cast<int>(v.busy);
            if (m.totalTime > 0) {
                ss.cpuBusyPercent = 100.0 * static_cast<double>(v.busy) / m.totalTime;
                if (cfg_.useResources) {
                    ss.avgCpuUtil = 100.0 * v.integral[0] / (static_cast<double>(v.total.cpu) * m.totalTime);
                    ss.avgRamUtil = 100.0 * v.integral[1] / (static_cast<double>(v.total.ramGb) * m.totalTime);
                    ss.avgStorageUtil = 100.0 * v.integral[2] / (static_cast<double>(v.total.storageGb) * m.totalTime);
                }
            }
            r.serverSummaries.push_back(ss);
        }
        if (m.totalTime > 0) {
            m.cpuUtilization = 100.0 * static_cast<double>(totalBusy) / (static_cast<double>(sv_.size()) * m.totalTime);
        }
        m.preemptions = preemptions_;

        std::stable_sort(r.segments.begin(), r.segments.end(), [](const Segment& a, const Segment& b) {
            if (a.serverId != b.serverId) return a.serverId < b.serverId;
            return a.start < b.start;
        });
        std::stable_sort(events_.begin(), events_.end(), [](const EngineEvent& a, const EngineEvent& b) { return a.time < b.time; });
        r.events = events_;
        r.allocations = allocations_;
        r.samples = samples_;
        if (rm_) r.decisionLog = rm_->logEntries();
        return r;
    }

    Algorithm alg_;
    SchedConfig cfg_;
    std::unique_ptr<ResourceManager> rm_;
    std::vector<PS> ps_;
    std::vector<SV> sv_;
    std::vector<EngineEvent> events_;
    std::vector<AllocationRecord> allocations_;
    std::vector<ResourceSample> samples_;
    long seqCounter_ = 0;
    int preemptions_ = 0;
};

}  // namespace

ScheduleResult runSchedule(const std::vector<SchedProcess>& processes, Algorithm algorithm, const SchedConfig& config) {
    ScheduleResult bad;
    bad.algorithm = algorithm;
    bad.errors = validateSchedule(processes, algorithm, config);
    if (!bad.errors.empty()) {
        bad.ok = false;
        return bad;
    }
    Engine engine(processes, algorithm, config);
    return engine.run();
}
