#include "ranking.h"

#include <cmath>

namespace {
constexpr double kTolerance = 1e-9;

struct MetricDef {
    const char* name;
    bool lowerIsBetter;
    double (*value)(const ScheduleMetrics&);
};

const MetricDef kMetrics[] = {
    {"avg_waiting", true, [](const ScheduleMetrics& m) { return m.avgWaiting; }},
    {"avg_turnaround", true, [](const ScheduleMetrics& m) { return m.avgTurnaround; }},
    {"avg_response", true, [](const ScheduleMetrics& m) { return m.avgResponse; }},
    {"throughput", false, [](const ScheduleMetrics& m) { return m.throughput; }},
    {"cpu_utilization", false, [](const ScheduleMetrics& m) { return m.cpuUtilization; }},
    {"context_switches", true, [](const ScheduleMetrics& m) { return static_cast<double>(m.contextSwitches); }},
};
}  // namespace

std::vector<BestForMetric> rankMetrics(const std::vector<RankedItem>& items) {
    std::vector<const RankedItem*> usable;
    for (const RankedItem& it : items) {
        if (it.metrics.completed > 0) usable.push_back(&it);
    }
    std::vector<BestForMetric> out;
    if (usable.empty()) return out;
    for (const MetricDef& def : kMetrics) {
        BestForMetric best;
        best.metric = def.name;
        best.lowerIsBetter = def.lowerIsBetter;
        double bestValue = def.value(usable[0]->metrics);
        double worstValue = bestValue;
        for (const RankedItem* it : usable) {
            const double v = def.value(it->metrics);
            if (def.lowerIsBetter ? v < bestValue - kTolerance : v > bestValue + kTolerance) bestValue = v;
            if (def.lowerIsBetter ? v > worstValue + kTolerance : v < worstValue - kTolerance) worstValue = v;
        }
        best.value = bestValue;
        for (const RankedItem* it : usable) {
            if (std::fabs(def.value(it->metrics) - bestValue) <= kTolerance) best.labels.push_back(it->label);
        }
        best.tie = best.labels.size() > 1;
        best.allEqual = std::fabs(worstValue - bestValue) <= kTolerance;
        out.push_back(best);
    }
    return out;
}
