#include "resource_manager.h"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace {

// Two scores closer than this are considered tied (floating point safety).
constexpr double kScoreEpsilon = 1e-9;

std::string fixed(double value, int decimals) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(decimals) << value;
    return out.str();
}

std::string percent(double ratio) {
    return fixed(ratio * 100.0, 0) + "%";
}

}  // namespace

std::string describeResources(const Resources& r) {
    return "CPU " + std::to_string(r.cpu) + ", RAM " + std::to_string(r.ramGb) + " GB, storage " +
           std::to_string(r.storageGb) + " GB";
}

ResourceManager::ResourceManager(std::vector<Server> servers) : servers_(std::move(servers)) {}

const Server* ResourceManager::findServer(int id) const {
    for (const Server& s : servers_) {
        if (s.id() == id) return &s;
    }
    return nullptr;
}

Server* ResourceManager::findServerMutable(int id) {
    for (Server& s : servers_) {
        if (s.id() == id) return &s;
    }
    return nullptr;
}

bool ResourceManager::fitsSomeServerTotal(const Resources& r) const {
    for (const Server& s : servers_) {
        if (r.cpu <= s.total().cpu && r.ramGb <= s.total().ramGb && r.storageGb <= s.total().storageGb) {
            return true;
        }
    }
    return false;
}

PlacementDecision ResourceManager::evaluate(const Job& job) const {
    PlacementDecision decision;
    double bestScore = 0.0;
    std::vector<std::string> blockers;

    // Servers are visited in ID order, so with a strict "<" comparison the
    // lowest ID automatically wins a tie.
    for (const Server& s : servers_) {
        ServerEvaluation eval;
        eval.serverId = s.id();
        if (s.canFit(job.demand)) {
            eval.eligible = true;
            eval.score = s.loadScoreAfter(job.demand);
            const Resources u = s.used();
            const double cpu = static_cast<double>(u.cpu + job.demand.cpu) / s.total().cpu;
            const double ram = static_cast<double>(u.ramGb + job.demand.ramGb) / s.total().ramGb;
            const double sto = static_cast<double>(u.storageGb + job.demand.storageGb) / s.total().storageGb;
            eval.detail = s.name() + " eligible: load score after placement " + fixed(eval.score, 3) +
                          " (CPU " + percent(cpu) + ", RAM " + percent(ram) + ", storage " + percent(sto) + ")";
            if (!decision.found || eval.score < bestScore - kScoreEpsilon) {
                decision.found = true;
                decision.serverId = s.id();
                bestScore = eval.score;
            }
        } else {
            eval.eligible = false;
            const std::string why = s.shortfall(job.demand);
            eval.detail = s.name() + " not eligible: short on " + why;
            blockers.push_back(s.name() + " short on " + why);
        }
        decision.evaluations.push_back(eval);
    }

    if (decision.found) {
        int tied = 0;  // other eligible servers with the same best score
        for (const ServerEvaluation& e : decision.evaluations) {
            if (e.eligible && e.serverId != decision.serverId && std::fabs(e.score - bestScore) <= kScoreEpsilon) {
                ++tied;
            }
        }
        const Server* chosen = findServer(decision.serverId);
        decision.summary = "Selected " + chosen->name() + ": lowest load score " + fixed(bestScore, 3) + " among eligible servers";
        if (tied > 0) decision.summary += " (tie with " + std::to_string(tied) + " other server(s); lowest server ID wins)";
    } else {
        decision.reason = "No server can currently fit " + describeResources(job.demand) + ": ";
        for (std::size_t i = 0; i < blockers.size(); ++i) {
            if (i > 0) decision.reason += "; ";
            decision.reason += blockers[i];
        }
        decision.summary = decision.reason;
    }
    return decision;
}

bool ResourceManager::allocate(Job& job, int serverId) {
    if (job.resourcesHeld) return false;  // already reserved: never reserve twice
    Server* server = findServerMutable(serverId);
    if (server == nullptr || !server->allocate(job.demand)) return false;
    job.serverId = serverId;
    job.resourcesHeld = true;
    return true;
}

bool ResourceManager::release(Job& job) {
    if (!job.resourcesHeld) return false;  // never reserved, or already released
    Server* server = findServerMutable(job.serverId);
    if (server == nullptr || !server->release(job.demand)) return false;
    job.resourcesHeld = false;
    return true;
}

double ResourceManager::overallUtilization() const {
    long usedCpu = 0, totalCpu = 0, usedRam = 0, totalRam = 0, usedSto = 0, totalSto = 0;
    for (const Server& s : servers_) {
        const Resources u = s.used();
        usedCpu += u.cpu;
        totalCpu += s.total().cpu;
        usedRam += u.ramGb;
        totalRam += s.total().ramGb;
        usedSto += u.storageGb;
        totalSto += s.total().storageGb;
    }
    if (totalCpu == 0 || totalRam == 0 || totalSto == 0) return 0.0;
    return (static_cast<double>(usedCpu) / totalCpu + static_cast<double>(usedRam) / totalRam +
            static_cast<double>(usedSto) / totalSto) / 3.0;
}

void ResourceManager::log(int time, int jobId, const std::string& message) {
    log_.push_back(LogEntry{time, jobId, message});
}

void ResourceManager::logPlacement(int time, const Job& job, const PlacementDecision& decision) {
    for (const ServerEvaluation& e : decision.evaluations) {
        log(time, job.id, "  " + e.detail);
    }
    const std::string who = "Job " + std::to_string(job.id) + ": ";
    log(time, job.id, who + (decision.found ? decision.summary : decision.summary + " -> job is WAITING and will be retried when resources are released"));
}
