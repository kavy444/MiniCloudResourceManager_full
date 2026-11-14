#include "report.h"

#include <algorithm>
#include <iomanip>
#include <string>
#include <vector>

namespace {

std::string cell(const std::string& text, std::size_t width) {
    if (text.size() >= width) return text.substr(0, width);
    return text + std::string(width - text.size(), ' ');
}

std::string usage(int used, int total, const char* unit) {
    return std::to_string(used) + "/" + std::to_string(total) + unit;
}

}  // namespace

void printServers(std::ostream& os, const ResourceManager& resources) {
    os << cell("Server", 10) << cell("CPU used/total", 16) << cell("RAM used/total", 18)
       << cell("Storage used/total", 22) << "Load\n";
    for (const Server& s : resources.servers()) {
        const Resources u = s.used();
        os << cell(s.name(), 10) << cell(usage(u.cpu, s.total().cpu, " cores"), 16)
           << cell(usage(u.ramGb, s.total().ramGb, " GB"), 18)
           << cell(usage(u.storageGb, s.total().storageGb, " GB"), 22) << std::fixed << std::setprecision(0)
           << s.overallUtilization() * 100.0 << "%\n";
        os << cell("", 10) << cell("free " + std::to_string(s.available().cpu), 16)
           << cell("free " + std::to_string(s.available().ramGb) + " GB", 18)
           << "free " + std::to_string(s.available().storageGb) + " GB\n";
    }
    os << "Cluster utilization (avg of CPU/RAM/storage): " << std::fixed << std::setprecision(1)
       << resources.overallUtilization() * 100.0 << "%\n";
}

void printJobs(std::ostream& os, const Simulation& sim) {
    if (sim.jobs().empty()) {
        os << "(no jobs submitted yet)\n";
        return;
    }
    os << cell("ID", 4) << cell("Name", 17) << cell("CPU", 5) << cell("RAM", 5) << cell("STO", 6) << cell("Dur", 5)
       << cell("Prio", 8) << cell("Arr", 5) << cell("State", 11) << cell("Server", 10) << cell("Start", 7)
       << cell("End", 6) << cell("Wait", 6) << "TAT\n";
    for (const Job& j : sim.jobs()) {
        auto num = [](int v) { return v < 0 ? std::string("-") : std::to_string(v); };
        const std::string server = j.serverId < 0 ? "-" : "S" + std::to_string(j.serverId);
        os << cell(std::to_string(j.id), 4) << cell(j.name, 17) << cell(std::to_string(j.demand.cpu), 5)
           << cell(std::to_string(j.demand.ramGb), 5) << cell(std::to_string(j.demand.storageGb), 6)
           << cell(std::to_string(j.duration), 5) << cell(toString(j.priority), 8)
           << cell(std::to_string(j.arrivalTime), 5) << cell(toString(j.state), 11) << cell(server, 10)
           << cell(num(j.startTime), 7) << cell(num(j.completionTime), 6) << cell(num(j.waitingTime()), 6)
           << num(j.turnaroundTime()) << "\n";
    }
    for (const Job& j : sim.jobs()) {
        if (!j.reason.empty() && (j.state == JobState::Waiting || j.state == JobState::Rejected)) {
            os << "  note J" << j.id << ": " << j.reason << "\n";
        }
    }
}

void printDecisionLog(std::ostream& os, const Simulation& sim) {
    if (sim.decisionLog().empty()) {
        os << "(decision log is empty)\n";
        return;
    }
    for (const LogEntry& e : sim.decisionLog()) {
        os << "[t=" << std::setw(2) << e.time << "] " << e.message << "\n";
    }
}

void printGantt(std::ostream& os, const Simulation& sim) {
    std::vector<TimelineEntry> rows = sim.timeline();
    if (rows.empty()) {
        os << "(no completed jobs yet, so there is no timeline)\n";
        return;
    }
    std::sort(rows.begin(), rows.end(), [](const TimelineEntry& a, const TimelineEntry& b) {
        if (a.serverId != b.serverId) return a.serverId < b.serverId;
        if (a.start != b.start) return a.start < b.start;
        return a.jobId < b.jobId;
    });
    int last = 0;
    for (const TimelineEntry& e : rows) last = std::max(last, e.end);
    const int scale = std::max(1, (last + 59) / 60);  // keep the chart <= 60 columns
    const int columns = (last + scale - 1) / scale;

    os << "Legend: '-' waiting (arrived, not started)   '#' running   1 column = " << scale << " tu\n";
    std::string ruler(static_cast<std::size_t>(columns) + 8, ' ');
    for (int c = 0; c < columns; c += 10) {
        const std::string label = std::to_string(c * scale);
        ruler.replace(static_cast<std::size_t>(c), label.size(), label);
    }
    os << cell("time (tu)", 24) << ruler << "\n";
    for (const TimelineEntry& e : rows) {
        os << cell("S" + std::to_string(e.serverId) + " J" + std::to_string(e.jobId) + " " + e.jobName, 24);
        for (int c = 0; c < columns; ++c) {
            const int t = c * scale;
            if (t >= e.start && t < e.end) os << '#';
            else if (t >= e.arrival && t < e.start) os << '-';
            else os << ' ';
        }
        os << "  [" << e.start << "," << e.end << ")\n";
    }
}

void printMetrics(std::ostream& os, const Simulation& sim) {
    const Metrics m = sim.metrics();
    os << "Policy: " << toString(sim.policy()) << "   (time unit: tu)\n";
    os << "  Total jobs submitted      : " << m.totalJobs << "\n";
    os << "  Completed jobs            : " << m.completedJobs << "\n";
    os << "  Rejected jobs             : " << m.rejectedJobs << "\n";
    os << "  Not finished              : " << m.unfinishedJobs << "\n";
    os << std::fixed << std::setprecision(3);
    os << "  Average waiting time      : " << m.avgWaitingTime << " tu  (mean of start - arrival, completed jobs)\n";
    os << "  Average turnaround time   : " << m.avgTurnaroundTime << " tu  (mean of completion - arrival)\n";
    os << "  Simulation duration       : " << m.simulationDuration << " tu  (last completion - first arrival)\n";
    os << "  Throughput                : " << m.throughput << " jobs/tu  (completed / duration)\n";
}
