// ranking.h - "which algorithm is best for each metric?" (never a single overall winner).
//
// Lower is better for: average waiting, average turnaround, average response, context switches.
// Higher is better for: throughput, CPU utilization.
// Runs that completed no process are ignored (their zero metrics would otherwise look "best").
// When several runs share the best value (compared with a 1e-9 tolerance) ALL of them are listed and
// `tie` is true - e.g. throughput is identical for every work-conserving algorithm on one CPU.
#pragma once

#include <string>
#include <vector>

#include "cpu_scheduler.h"

struct RankedItem {
    std::string label;
    ScheduleMetrics metrics;
};

struct BestForMetric {
    std::string metric;          // JSON-style name, e.g. "avg_waiting"
    bool lowerIsBetter = true;
    double value = 0;
    std::vector<std::string> labels;
    bool tie = false;            // more than one label shares the best value
    bool allEqual = false;       // every ranked item has the same value (the metric cannot discriminate)
};

std::vector<BestForMetric> rankMetrics(const std::vector<RankedItem>& items);
