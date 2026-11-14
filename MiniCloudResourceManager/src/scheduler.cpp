#include "scheduler.h"

#include <algorithm>
#include <cctype>

std::string toString(Policy p) {
    return p == Policy::FCFS ? "FCFS" : "Priority";
}

bool parsePolicy(const std::string& text, Policy& out) {
    std::string t;
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (t == "fcfs" || t == "1") { out = Policy::FCFS; return true; }
    if (t == "priority" || t == "2") { out = Policy::Priority; return true; }
    return false;
}

bool Scheduler::comesBefore(const Job& a, const Job& b) const {
    if (policy_ == Policy::Priority) {
        const int rankA = priorityRank(a.priority);
        const int rankB = priorityRank(b.priority);
        if (rankA != rankB) return rankA < rankB;  // more urgent first
    }
    // FCFS rule (also the tie-breaker for Priority): arrival, then submission id.
    if (a.arrivalTime != b.arrivalTime) return a.arrivalTime < b.arrivalTime;
    return a.id < b.id;
}

void Scheduler::sortReadyQueue(std::vector<Job*>& ready) const {
    std::sort(ready.begin(), ready.end(),
              [this](const Job* a, const Job* b) { return comesBefore(*a, *b); });
}
