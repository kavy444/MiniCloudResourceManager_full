#include <algorithm>
#include <stdexcept>
#include "engine_api.h"

#include <set>

#include "banker.h"
#include "cpu_scheduler.h"
#include "memory_manager.h"
#include "ranking.h"
#include "resource_manager.h"
#include "sync_demo.h"

namespace {

// ---------------------------------------------------------------------------------------------
// request reading helpers (strict types, error accumulation)
// ---------------------------------------------------------------------------------------------
struct Reader {
    std::vector<std::string> errors;

    bool isWholeNumber(const Json& v) const {
        if (v.isInt()) return true;
        if (v.type() == Json::Type::Double) {
            const double d = v.asDouble();
            return d == static_cast<double>(static_cast<long long>(d));
        }
        return false;
    }

    // Reads an integer field. Missing -> `fallback` when !required. Wrong type -> error.
    bool integer(const Json& obj, const std::string& key, int& out, bool required, int fallback, const std::string& where) {
        const Json* v = obj.find(key);
        if (v == nullptr || v->isNull()) {
            if (required) {
                errors.push_back(where + "'" + key + "' is required");
                return false;
            }
            out = fallback;
            return true;
        }
        if (!isWholeNumber(*v)) {
            errors.push_back(where + "'" + key + "' must be a whole number");
            return false;
        }
        const long long n = v->asInt();
        if (n < -2000000000LL || n > 2000000000LL) {
            errors.push_back(where + "'" + key + "' is out of range");
            return false;
        }
        out = static_cast<int>(n);
        return true;
    }

    bool text(const Json& obj, const std::string& key, std::string& out, bool required, const std::string& fallback, const std::string& where) {
        const Json* v = obj.find(key);
        if (v == nullptr || v->isNull()) {
            if (required) {
                errors.push_back(where + "'" + key + "' is required");
                return false;
            }
            out = fallback;
            return true;
        }
        if (!v->isString()) {
            errors.push_back(where + "'" + key + "' must be a string");
            return false;
        }
        out = v->asString();
        return true;
    }

    bool boolean(const Json& obj, const std::string& key, bool& out, bool fallback, const std::string& where) {
        const Json* v = obj.find(key);
        if (v == nullptr || v->isNull()) {
            out = fallback;
            return true;
        }
        if (!v->isBool()) {
            errors.push_back(where + "'" + key + "' must be true or false");
            return false;
        }
        out = v->asBool();
        return true;
    }

    const Json* array(const Json& obj, const std::string& key, bool required, const std::string& where) {
        const Json* v = obj.find(key);
        if (v == nullptr || v->isNull()) {
            if (required) errors.push_back(where + "'" + key + "' is required");
            return nullptr;
        }
        if (!v->isArray()) {
            errors.push_back(where + "'" + key + "' must be a list");
            return nullptr;
        }
        return v;
    }

    bool intList(const Json& value, std::vector<int>& out, const std::string& where) {
        out.clear();
        if (!value.isArray()) {
            errors.push_back(where + "must be a list of whole numbers");
            return false;
        }
        for (const Json& item : value.items()) {
            if (!isWholeNumber(item)) {
                errors.push_back(where + "must contain only whole numbers");
                return false;
            }
            out.push_back(static_cast<int>(item.asInt()));
        }
        return true;
    }

    bool intMatrix(const Json& value, IntMatrix& out, const std::string& where) {
        out.clear();
        if (!value.isArray()) {
            errors.push_back(where + "must be a list of lists");
            return false;
        }
        for (const Json& row : value.items()) {
            std::vector<int> r;
            if (!intList(row, r, where + "rows ")) return false;
            out.push_back(r);
        }
        return true;
    }
};

Json fail(const std::vector<std::string>& errors) {
    Json out = Json::object();
    out.set("ok", false);
    Json list = Json::array();
    for (const std::string& e : errors) list.push(e);
    out.set("errors", list);
    return out;
}

Json fail(const std::string& error) { return fail(std::vector<std::string>{error}); }

Json intArray(const std::vector<int>& values) {
    Json a = Json::array();
    for (int v : values) a.push(v);
    return a;
}

Json intMatrixJson(const IntMatrix& m) {
    Json a = Json::array();
    for (const auto& row : m) a.push(intArray(row));
    return a;
}

Json stringArray(const std::vector<std::string>& values) {
    Json a = Json::array();
    for (const std::string& v : values) a.push(v);
    return a;
}

// ---------------------------------------------------------------------------------------------
// scheduling
// ---------------------------------------------------------------------------------------------
bool parseProcesses(Reader& rd, const Json& req, std::vector<SchedProcess>& out) {
    const Json* list = rd.array(req, "processes", true, "");
    if (list == nullptr) return false;
    std::size_t index = 0;
    for (const Json& item : list->items()) {
        const std::string where = "processes[" + std::to_string(index++) + "]: ";
        if (!item.isObject()) {
            rd.errors.push_back(where + "must be an object");
            continue;
        }
        SchedProcess p;
        rd.integer(item, "id", p.id, true, 0, where);
        rd.text(item, "name", p.name, false, "", where);
        rd.integer(item, "arrival", p.arrival, true, 0, where);
        rd.integer(item, "burst", p.burst, true, 0, where);
        rd.integer(item, "priority", p.priority, false, 2, where);
        rd.integer(item, "queue", p.queue, false, -1, where);
        rd.integer(item, "cpu", p.demand.cpu, false, 1, where);
        rd.integer(item, "ram_gb", p.demand.ramGb, false, 1, where);
        rd.integer(item, "storage_gb", p.demand.storageGb, false, 1, where);
        out.push_back(p);
    }
    return rd.errors.empty();
}

bool parseServers(Reader& rd, const Json& list, std::vector<Server>& out, const std::string& where) {
    std::set<int> ids;
    std::size_t index = 0;
    for (const Json& item : list.items()) {
        const std::string w = where + "[" + std::to_string(index++) + "]: ";
        if (!item.isObject()) {
            rd.errors.push_back(w + "must be an object");
            continue;
        }
        int id = 0;
        std::string name;
        Resources total;
        rd.integer(item, "id", id, true, 0, w);
        rd.text(item, "name", name, false, "", w);
        rd.integer(item, "cpu", total.cpu, true, 0, w);
        rd.integer(item, "ram_gb", total.ramGb, true, 0, w);
        rd.integer(item, "storage_gb", total.storageGb, true, 0, w);
        if (id < 1) rd.errors.push_back(w + "id must be at least 1");
        if (!ids.insert(id).second) rd.errors.push_back(w + "duplicate server id");
        if (name.empty()) name = "Server-" + std::to_string(id);
        try {
            out.emplace_back(id, name, total);
        } catch (const std::invalid_argument&) {
            rd.errors.push_back(w + "cpu, ram_gb and storage_gb must all be positive");
        }
    }
    return rd.errors.empty();
}

bool parseConfig(Reader& rd, const Json& req, SchedConfig& cfg) {
    const Json* c = req.find("config");
    if (c == nullptr || c->isNull()) return true;
    if (!c->isObject()) {
        rd.errors.push_back("'config' must be an object");
        return false;
    }
    rd.integer(*c, "time_quantum", cfg.timeQuantum, false, cfg.timeQuantum, "config: ");

    if (const Json* mlq = rd.array(*c, "mlq", false, "config: ")) {
        cfg.mlq.clear();
        std::size_t i = 0;
        for (const Json& q : mlq->items()) {
            const std::string w = "config.mlq[" + std::to_string(i++) + "]: ";
            if (!q.isObject()) {
                rd.errors.push_back(w + "must be an object");
                continue;
            }
            QueueSpec spec;
            std::string policy;
            rd.text(q, "policy", policy, true, "", w);
            if (!policy.empty() && !parseQueuePolicy(policy, spec.policy)) {
                rd.errors.push_back(w + "unknown policy '" + policy + "' (use FCFS, RR, SJF or PRIORITY)");
            }
            rd.integer(q, "quantum", spec.quantum, false, 0, w);
            cfg.mlq.push_back(spec);
        }
    }
    if (const Json* mlfq = c->find("mlfq")) {
        if (!mlfq->isNull() && !mlfq->isObject()) {
            rd.errors.push_back("config.mlfq must be an object");
        } else if (mlfq->isObject()) {
            if (const Json* quanta = mlfq->find("quanta")) {
                std::vector<int> q;
                if (rd.intList(*quanta, q, "config.mlfq.quanta ")) cfg.mlfqQuanta = q;
            }
            rd.integer(*mlfq, "aging", cfg.agingThreshold, false, 0, "config.mlfq: ");
        }
    }
    if (const Json* cluster = c->find("cluster")) {
        if (!cluster->isNull() && !cluster->isObject()) {
            rd.errors.push_back("config.cluster must be an object");
        } else if (cluster->isObject()) {
            rd.boolean(*cluster, "enabled", cfg.useResources, false, "config.cluster: ");
            if (cfg.useResources) {
                if (const Json* servers = rd.array(*cluster, "servers", true, "config.cluster: ")) {
                    parseServers(rd, *servers, cfg.servers, "config.cluster.servers");
                }
            }
        }
    }
    return rd.errors.empty();
}

Json metricsJson(const ScheduleMetrics& m) {
    Json j = Json::object();
    j.set("total_processes", m.totalProcesses);
    j.set("completed", m.completed);
    j.set("rejected", m.rejected);
    j.set("avg_waiting", m.avgWaiting);
    j.set("avg_turnaround", m.avgTurnaround);
    j.set("avg_response", m.avgResponse);
    j.set("total_time", m.totalTime);
    j.set("start_time", m.startTime);
    j.set("throughput", m.throughput);
    j.set("cpu_utilization", m.cpuUtilization);
    j.set("context_switches", m.contextSwitches);
    j.set("preemptions", m.preemptions);
    return j;
}

Json resultJson(const ScheduleResult& r) {
    Json out = Json::object();
    out.set("ok", r.ok);
    out.set("algorithm", algorithmName(r.algorithm));
    if (!r.ok) {
        Json list = Json::array();
        for (const std::string& e : r.errors) list.push(e);
        out.set("errors", list);
        return out;
    }
    out.set("metrics", metricsJson(r.metrics));

    Json procs = Json::array();
    for (const ProcessResult& p : r.processes) {
        Json j = Json::object();
        j.set("id", p.id);
        j.set("name", p.name);
        j.set("arrival", p.arrival);
        j.set("burst", p.burst);
        j.set("priority", p.priority);
        j.set("status", p.status);
        j.set("reason", p.reason);
        j.set("server_id", p.serverId);
        j.set("admitted_at", p.admittedAt);
        j.set("first_start", p.firstStart);
        j.set("completion", p.completion);
        j.set("turnaround", p.turnaround);
        j.set("waiting", p.waiting);
        j.set("response", p.response);
        procs.push(j);
    }
    out.set("processes", procs);

    Json segs = Json::array();
    for (const Segment& s : r.segments) {
        Json j = Json::object();
        j.set("process_id", s.processId);
        j.set("server_id", s.serverId);
        j.set("start", s.start);
        j.set("end", s.end);
        j.set("type", s.type);
        j.set("end_reason", s.endReason);
        j.set("queue_level", s.queueLevel);
        segs.push(j);
    }
    out.set("segments", segs);

    Json events = Json::array();
    for (const EngineEvent& e : r.events) {
        Json j = Json::object();
        j.set("time", e.time);
        j.set("type", e.type);
        j.set("process_id", e.processId);
        j.set("server_id", e.serverId);
        j.set("description", e.description);
        events.push(j);
    }
    out.set("events", events);

    Json allocs = Json::array();
    for (const AllocationRecord& a : r.allocations) {
        Json j = Json::object();
        j.set("process_id", a.processId);
        j.set("server_id", a.serverId);
        j.set("cpu", a.amount.cpu);
        j.set("ram_gb", a.amount.ramGb);
        j.set("storage_gb", a.amount.storageGb);
        j.set("allocated_at", a.allocatedAt);
        j.set("released_at", a.releasedAt);
        allocs.push(j);
    }
    out.set("allocations", allocs);

    Json samples = Json::array();
    for (const ResourceSample& s : r.samples) {
        Json j = Json::object();
        j.set("time", s.time);
        Json servers = Json::array();
        for (const ServerUsage& u : s.servers) {
            Json sj = Json::object();
            sj.set("server_id", u.serverId);
            sj.set("cpu", u.used.cpu);
            sj.set("ram_gb", u.used.ramGb);
            sj.set("storage_gb", u.used.storageGb);
            servers.push(sj);
        }
        j.set("servers", servers);
        samples.push(j);
    }
    out.set("samples", samples);

    Json summaries = Json::array();
    for (const ServerSummary& s : r.serverSummaries) {
        Json j = Json::object();
        j.set("server_id", s.serverId);
        j.set("name", s.name);
        j.set("total_cpu", s.total.cpu);
        j.set("total_ram_gb", s.total.ramGb);
        j.set("total_storage_gb", s.total.storageGb);
        j.set("peak_cpu", s.peakUsed.cpu);
        j.set("peak_ram_gb", s.peakUsed.ramGb);
        j.set("peak_storage_gb", s.peakUsed.storageGb);
        j.set("avg_cpu_util", s.avgCpuUtil);
        j.set("avg_ram_util", s.avgRamUtil);
        j.set("avg_storage_util", s.avgStorageUtil);
        j.set("cpu_busy_percent", s.cpuBusyPercent);
        j.set("busy_time", s.busyTime);
        summaries.push(j);
    }
    out.set("server_summaries", summaries);

    Json log = Json::array();
    for (const LogEntry& e : r.decisionLog) {
        Json j = Json::object();
        j.set("time", e.time);
        j.set("process_id", e.jobId);
        j.set("message", e.message);
        log.push(j);
    }
    out.set("decision_log", log);
    return out;
}

Json bestJson(const std::vector<BestForMetric>& best) {
    Json arr = Json::array();
    for (const BestForMetric& b : best) {
        Json j = Json::object();
        j.set("metric", b.metric);
        j.set("lower_is_better", b.lowerIsBetter);
        j.set("value", b.value);
        j.set("best", stringArray(b.labels));
        j.set("tie", b.tie);
        j.set("all_equal", b.allEqual);
        arr.push(j);
    }
    return arr;
}

Json cmdSchedule(const Json& req) {
    Reader rd;
    std::string name;
    rd.text(req, "algorithm", name, true, "", "");
    Algorithm alg = Algorithm::FCFS;
    if (!name.empty() && !parseAlgorithm(name, alg)) rd.errors.push_back("unknown algorithm '" + name + "'");
    std::vector<SchedProcess> procs;
    parseProcesses(rd, req, procs);
    SchedConfig cfg;
    parseConfig(rd, req, cfg);
    if (!rd.errors.empty()) return fail(rd.errors);
    return resultJson(runSchedule(procs, alg, cfg));
}

Json cmdCompare(const Json& req) {
    Reader rd;
    std::vector<Algorithm> algs;
    if (const Json* list = rd.array(req, "algorithms", true, "")) {
        for (const Json& a : list->items()) {
            Algorithm alg = Algorithm::FCFS;
            if (!a.isString() || !parseAlgorithm(a.asString(), alg)) {
                rd.errors.push_back("algorithms must be names such as FCFS, SJF, RR");
                break;
            }
            if (std::find(algs.begin(), algs.end(), alg) != algs.end()) {
                rd.errors.push_back("algorithm " + a.asString() + " listed twice");
                break;
            }
            algs.push_back(alg);
        }
        if (algs.empty() && rd.errors.empty()) rd.errors.push_back("at least one algorithm is required");
    }
    std::vector<SchedProcess> procs;
    parseProcesses(rd, req, procs);
    SchedConfig cfg;
    parseConfig(rd, req, cfg);
    if (!rd.errors.empty()) return fail(rd.errors);

    Json results = Json::array();
    std::vector<RankedItem> ranked;
    bool allOk = true;
    for (Algorithm a : algs) {
        const ScheduleResult r = runSchedule(procs, a, cfg);
        results.push(resultJson(r));
        if (r.ok) ranked.push_back(RankedItem{algorithmName(a), r.metrics});
        else allOk = false;
    }
    Json out = Json::object();
    out.set("ok", allOk);
    out.set("results", results);
    out.set("best", bestJson(rankMetrics(ranked)));
    return out;
}

Json cmdRank(const Json& req) {
    Reader rd;
    std::vector<RankedItem> items;
    if (const Json* list = rd.array(req, "items", true, "")) {
        std::size_t i = 0;
        for (const Json& it : list->items()) {
            const std::string w = "items[" + std::to_string(i++) + "]: ";
            if (!it.isObject()) {
                rd.errors.push_back(w + "must be an object");
                continue;
            }
            RankedItem ri;
            rd.text(it, "label", ri.label, true, "", w);
            const Json* m = it.find("metrics");
            if (m == nullptr || !m->isObject()) {
                rd.errors.push_back(w + "'metrics' must be an object");
                continue;
            }
            auto num = [&](const char* key, double& out) {
                const Json* v = m->find(key);
                if (v == nullptr || !v->isNumber()) {
                    rd.errors.push_back(w + "metrics." + key + " must be a number");
                    return;
                }
                out = v->asDouble();
            };
            double completed = 0, switches = 0;
            num("completed", completed);
            num("avg_waiting", ri.metrics.avgWaiting);
            num("avg_turnaround", ri.metrics.avgTurnaround);
            num("avg_response", ri.metrics.avgResponse);
            num("throughput", ri.metrics.throughput);
            num("cpu_utilization", ri.metrics.cpuUtilization);
            num("context_switches", switches);
            ri.metrics.completed = static_cast<int>(completed);
            ri.metrics.contextSwitches = static_cast<int>(switches);
            items.push_back(ri);
        }
    }
    if (!rd.errors.empty()) return fail(rd.errors);
    Json out = Json::object();
    out.set("ok", true);
    out.set("best", bestJson(rankMetrics(items)));
    return out;
}

// ---------------------------------------------------------------------------------------------
// placement of one job on live server state
// ---------------------------------------------------------------------------------------------
Json cmdPlace(const Json& req) {
    Reader rd;
    std::vector<Server> servers;
    std::vector<Resources> used;
    if (const Json* list = rd.array(req, "servers", true, "")) {
        parseServers(rd, *list, servers, "servers");
        std::size_t i = 0;
        for (const Json& item : list->items()) {
            Resources u;
            const std::string w = "servers[" + std::to_string(i++) + "]: ";
            if (item.isObject()) {
                rd.integer(item, "used_cpu", u.cpu, false, 0, w);
                rd.integer(item, "used_ram_gb", u.ramGb, false, 0, w);
                rd.integer(item, "used_storage_gb", u.storageGb, false, 0, w);
            }
            used.push_back(u);
        }
    }
    const Json* jobJson = req.find("job");
    Job job;
    if (jobJson == nullptr || !jobJson->isObject()) {
        rd.errors.push_back("'job' must be an object");
    } else {
        rd.integer(*jobJson, "id", job.id, false, 1, "job: ");
        rd.text(*jobJson, "name", job.name, false, "", "job: ");
        rd.integer(*jobJson, "cpu", job.demand.cpu, true, 0, "job: ");
        rd.integer(*jobJson, "ram_gb", job.demand.ramGb, true, 0, "job: ");
        rd.integer(*jobJson, "storage_gb", job.demand.storageGb, true, 0, "job: ");
        if (job.demand.cpu < 1 || job.demand.ramGb < 1 || job.demand.storageGb < 1) {
            rd.errors.push_back("job: cpu, ram_gb and storage_gb must each be at least 1");
        }
    }
    if (rd.errors.empty() && servers.empty()) rd.errors.push_back("at least one server is required");
    if (!rd.errors.empty()) return fail(rd.errors);

    for (std::size_t i = 0; i < servers.size(); ++i) {
        if (used[i].cpu < 0 || used[i].ramGb < 0 || used[i].storageGb < 0 || !servers[i].allocate(used[i])) {
            return fail("servers[" + std::to_string(i) + "]: used amounts must be between 0 and the server's total");
        }
    }
    ResourceManager rm(servers);
    Json out = Json::object();
    out.set("ok", true);
    out.set("fits_some_total", rm.fitsSomeServerTotal(job.demand));
    const PlacementDecision d = rm.evaluate(job);
    out.set("found", d.found);
    out.set("server_id", d.found ? d.serverId : -1);
    out.set("summary", d.summary);
    out.set("reason", d.reason);
    Json evals = Json::array();
    for (const ServerEvaluation& e : d.evaluations) {
        Json j = Json::object();
        j.set("server_id", e.serverId);
        j.set("eligible", e.eligible);
        j.set("score", e.eligible ? Json(e.score) : Json());
        j.set("detail", e.detail);
        evals.push(j);
    }
    out.set("evaluations", evals);
    return out;
}

// ---------------------------------------------------------------------------------------------
// memory management
// ---------------------------------------------------------------------------------------------
Json blocksJson(const std::vector<MemoryBlock>& blocks) {
    Json arr = Json::array();
    for (const MemoryBlock& b : blocks) {
        Json j = Json::object();
        j.set("start", b.start);
        j.set("size", b.size);
        j.set("free", b.free);
        j.set("owner", b.owner);
        j.set("requested", b.requested);
        arr.push(j);
    }
    return arr;
}

Json memoryStatsJson(const MemoryStats& s) {
    Json j = Json::object();
    j.set("total", s.total);
    j.set("allocated", s.allocated);
    j.set("free", s.free);
    j.set("largest_free", s.largestFree);
    j.set("free_blocks", s.freeBlocks);
    j.set("external_fragmentation", s.externalFragmentation);
    j.set("internal_fragmentation", s.internalFragmentation);
    j.set("utilization_percent", s.utilizationPercent);
    return j;
}

Json cmdMemory(const Json& req) {
    Reader rd;
    MemoryConfig cfg;
    std::string strategy;
    rd.text(req, "strategy", strategy, true, "", "");
    if (!strategy.empty() && !parseFitStrategy(strategy, cfg.strategy)) {
        rd.errors.push_back("unknown strategy '" + strategy + "' (use FIRST_FIT, BEST_FIT or WORST_FIT)");
    }
    if (const Json* parts = req.find("partitions")) {
        if (!parts->isNull()) rd.intList(*parts, cfg.partitions, "'partitions' ");
    }
    if (cfg.partitions.empty()) rd.integer(req, "total_size", cfg.totalSize, true, 0, "");
    std::vector<MemoryOp> ops;
    if (const Json* list = rd.array(req, "operations", true, "")) {
        std::size_t i = 0;
        for (const Json& item : list->items()) {
            const std::string w = "operations[" + std::to_string(i++) + "]: ";
            if (!item.isObject()) {
                rd.errors.push_back(w + "must be an object");
                continue;
            }
            MemoryOp op;
            rd.text(item, "type", op.type, true, "", w);
            rd.text(item, "process", op.process, true, "", w);
            rd.integer(item, "size", op.size, false, 0, w);
            ops.push_back(op);
        }
    }
    if (!rd.errors.empty()) return fail(rd.errors);
    const MemoryResult r = simulateMemory(cfg, ops);
    if (!r.ok) return fail(r.errors);

    Json out = Json::object();
    out.set("ok", true);
    out.set("strategy", fitStrategyName(r.strategy));
    out.set("fixed_partitions", r.fixedPartitions);
    out.set("failed_allocations", r.failedAllocations);
    Json steps = Json::array();
    for (const MemoryStep& s : r.steps) {
        Json j = Json::object();
        j.set("index", s.index);
        j.set("type", s.op.type);
        j.set("process", s.op.process);
        j.set("size", s.op.size);
        j.set("success", s.success);
        j.set("message", s.message);
        j.set("block_start", s.blockStart);
        j.set("blocks", blocksJson(s.blocks));
        j.set("stats", memoryStatsJson(s.stats));
        steps.push(j);
    }
    out.set("steps", steps);
    out.set("final_stats", memoryStatsJson(r.finalStats));
    return out;
}

// ---------------------------------------------------------------------------------------------
// deadlock handling
// ---------------------------------------------------------------------------------------------
Json safetyJson(const SafetyResult& s) {
    Json j = Json::object();
    j.set("safe", s.safe);
    j.set("sequence", intArray(s.sequence));
    j.set("unfinished", intArray(s.unfinished));
    j.set("need", intMatrixJson(s.need));
    j.set("explanation", s.explanation);
    Json steps = Json::array();
    for (const SafetyStep& st : s.steps) {
        Json sj = Json::object();
        sj.set("process", st.process);
        sj.set("need", intArray(st.need));
        sj.set("work_before", intArray(st.workBefore));
        sj.set("work_after", intArray(st.workAfter));
        steps.push(sj);
    }
    j.set("steps", steps);
    return j;
}

Json cmdBanker(const Json& req) {
    Reader rd;
    std::string action;
    rd.text(req, "action", action, true, "", "");
    std::vector<int> available;
    if (const Json* v = req.find("available")) rd.intList(*v, available, "'available' ");
    else rd.errors.push_back("'available' is required");

    if (action == "detect") {
        IntMatrix allocation, request;
        if (const Json* v = req.find("allocation")) rd.intMatrix(*v, allocation, "'allocation' ");
        else rd.errors.push_back("'allocation' is required");
        if (const Json* v = req.find("request")) rd.intMatrix(*v, request, "'request' ");
        else rd.errors.push_back("'request' is required");
        if (!rd.errors.empty()) return fail(rd.errors);
        const std::vector<std::string> errors = validateDetectionInput(available, allocation, request);
        if (!errors.empty()) return fail(errors);
        const DetectionResult d = detectDeadlock(available, allocation, request);
        Json out = Json::object();
        out.set("ok", true);
        out.set("action", "detect");
        out.set("deadlocked", d.deadlocked);
        out.set("deadlocked_processes", intArray(d.deadlockedProcesses));
        out.set("finish_order", intArray(d.finishOrder));
        out.set("explanation", d.explanation);
        return out;
    }
    if (action != "safety" && action != "request") {
        rd.errors.push_back("action must be 'safety', 'request' or 'detect'");
    }
    BankerState state;
    state.available = available;
    if (const Json* v = req.find("max")) rd.intMatrix(*v, state.maxDemand, "'max' ");
    else rd.errors.push_back("'max' is required");
    if (const Json* v = req.find("allocation")) rd.intMatrix(*v, state.allocation, "'allocation' ");
    else rd.errors.push_back("'allocation' is required");
    if (!rd.errors.empty()) return fail(rd.errors);
    const std::vector<std::string> errors = validateBankerState(state);
    if (!errors.empty()) return fail(errors);

    Json out = Json::object();
    out.set("ok", true);
    out.set("action", action);
    if (action == "safety") {
        out.set("safety", safetyJson(checkSafety(state)));
        return out;
    }
    int process = 0;
    std::vector<int> request;
    rd.integer(req, "process", process, true, 0, "");
    if (const Json* v = req.find("request")) rd.intList(*v, request, "'request' ");
    else rd.errors.push_back("'request' is required");
    if (!rd.errors.empty()) return fail(rd.errors);
    const RequestOutcome o = requestResources(state, process, request);
    out.set("granted", o.granted);
    out.set("reason", o.reason);
    out.set("safety_evaluated", o.safetyEvaluated);
    out.set("safety_if_granted", o.safetyEvaluated ? safetyJson(o.safetyIfGranted) : Json());
    out.set("safety_before", safetyJson(checkSafety(state)));
    return out;
}

// ---------------------------------------------------------------------------------------------
// synchronization demos
// ---------------------------------------------------------------------------------------------
Json cmdSync(const Json& req) {
    Reader rd;
    std::string scenario;
    rd.text(req, "scenario", scenario, true, "", "");
    SyncResult r;
    if (scenario == "race") {
        int n = 0;
        bool useMutex = false;
        rd.integer(req, "increments", n, false, 5, "");
        rd.boolean(req, "use_mutex", useMutex, false, "");
        if (!rd.errors.empty()) return fail(rd.errors);
        r = raceCondition(n, useMutex);
    } else if (scenario == "producer_consumer") {
        int items = 0, buffer = 0, delay = 0;
        bool wrong = false;
        rd.integer(req, "items", items, false, 6, "");
        rd.integer(req, "buffer_size", buffer, false, 2, "");
        rd.boolean(req, "wrong_order", wrong, false, "");
        rd.integer(req, "consumer_delay", delay, false, 0, "");
        if (!rd.errors.empty()) return fail(rd.errors);
        r = producerConsumer(items, buffer, wrong, delay);
    } else {
        if (!scenario.empty()) rd.errors.push_back("scenario must be 'race' or 'producer_consumer'");
        return fail(rd.errors);
    }
    if (!r.ok) return fail(r.errors);

    Json out = Json::object();
    out.set("ok", true);
    out.set("scenario", r.scenario);
    out.set("semaphores", stringArray(r.semaphoreNames));
    out.set("deadlock", r.deadlock);
    out.set("truncated", r.truncated);
    out.set("expected_counter", r.expectedCounter);
    out.set("final_counter", r.finalCounter);
    out.set("lost_updates", r.lostUpdates);
    out.set("produced", r.produced);
    out.set("consumed", r.consumed);
    out.set("max_buffer_occupancy", r.maxBufferOccupancy);
    out.set("consumed_in_order", r.consumedInOrder);
    out.set("summary", r.summary);
    Json steps = Json::array();
    for (const SyncStep& s : r.steps) {
        Json j = Json::object();
        j.set("step", s.step);
        j.set("round", s.round);
        j.set("thread", s.threadName);
        j.set("action", s.action);
        j.set("detail", s.detail);
        j.set("counter", s.counter);
        j.set("buffer", intArray(s.buffer));
        j.set("semaphores", intArray(s.semaphores));
        steps.push(j);
    }
    out.set("steps", steps);
    return out;
}

Json cmdInfo() {
    Json out = Json::object();
    out.set("ok", true);
    out.set("protocol", kEngineProtocolVersion);
    Json algs = Json::array();
    for (Algorithm a : allAlgorithms()) algs.push(algorithmName(a));
    out.set("algorithms", algs);
    out.set("queue_policies", stringArray({"FCFS", "RR", "SJF", "PRIORITY"}));
    out.set("fit_strategies", stringArray({"FIRST_FIT", "BEST_FIT", "WORST_FIT"}));
    out.set("commands", stringArray({"info", "schedule", "compare", "rank", "place", "memory", "banker", "sync"}));
    out.set("priority_rule", "smaller number = higher priority");
    return out;
}

}  // namespace

Json handleRequest(const Json& request) {
    if (!request.isObject()) return fail("request must be a JSON object");
    Reader rd;
    std::string command;
    if (!rd.text(request, "command", command, true, "", "")) return fail(rd.errors);
    if (command == "info") return cmdInfo();
    if (command == "schedule") return cmdSchedule(request);
    if (command == "compare") return cmdCompare(request);
    if (command == "rank") return cmdRank(request);
    if (command == "place") return cmdPlace(request);
    if (command == "memory") return cmdMemory(request);
    if (command == "banker") return cmdBanker(request);
    if (command == "sync") return cmdSync(request);
    return fail("unknown command '" + command + "'");
}

std::string handleRequestText(const std::string& text) {
    Json request;
    std::string error;
    if (!Json::parse(text, request, error)) return fail("invalid JSON: " + error).dump();
    return handleRequest(request).dump();
}
