#include "banker.h"

namespace {
constexpr std::size_t kMaxProcesses = 50;
constexpr std::size_t kMaxResources = 20;
constexpr int kMaxInstances = 1000000;

bool lessOrEqual(const std::vector<int>& a, const std::vector<int>& b) {
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] > b[i]) return false;
    }
    return true;
}

IntMatrix computeNeed(const BankerState& s) {
    IntMatrix need(s.maxDemand.size());
    for (std::size_t i = 0; i < s.maxDemand.size(); ++i) {
        need[i].resize(s.maxDemand[i].size());
        for (std::size_t j = 0; j < s.maxDemand[i].size(); ++j) need[i][j] = s.maxDemand[i][j] - s.allocation[i][j];
    }
    return need;
}

std::vector<std::string> shapeErrors(const std::vector<int>& available, const IntMatrix& a, const IntMatrix& b,
                                     const char* nameA, const char* nameB) {
    std::vector<std::string> errors;
    const std::size_t m = available.size();
    if (m < 1 || m > kMaxResources) errors.push_back("number of resource types must be between 1 and 20");
    if (a.empty() || a.size() > kMaxProcesses) errors.push_back("number of processes must be between 1 and 50");
    if (a.size() != b.size()) errors.push_back(std::string(nameA) + " and " + nameB + " must have the same number of rows");
    for (int v : available) {
        if (v < 0 || v > kMaxInstances) errors.push_back("available values must be between 0 and " + std::to_string(kMaxInstances));
    }
    auto checkMatrix = [&](const IntMatrix& mat, const char* name) {
        for (std::size_t i = 0; i < mat.size(); ++i) {
            if (mat[i].size() != m) {
                errors.push_back(std::string(name) + " row " + std::to_string(i) + " must have " + std::to_string(m) + " values");
                continue;
            }
            for (int v : mat[i]) {
                if (v < 0 || v > kMaxInstances) {
                    errors.push_back(std::string(name) + " row " + std::to_string(i) + ": values must be between 0 and " +
                                     std::to_string(kMaxInstances));
                    break;
                }
            }
        }
    };
    checkMatrix(a, nameA);
    checkMatrix(b, nameB);
    return errors;
}
}  // namespace

std::vector<std::string> validateBankerState(const BankerState& s) {
    std::vector<std::string> errors = shapeErrors(s.available, s.maxDemand, s.allocation, "max", "allocation");
    if (!errors.empty()) return errors;
    for (std::size_t i = 0; i < s.maxDemand.size(); ++i) {
        for (std::size_t j = 0; j < s.available.size(); ++j) {
            if (s.allocation[i][j] > s.maxDemand[i][j]) {
                errors.push_back("process " + std::to_string(i) + ", resource " + std::to_string(j) +
                                 ": allocation exceeds the declared maximum");
            }
        }
    }
    return errors;
}

SafetyResult checkSafety(const BankerState& state) {
    SafetyResult result;
    const std::size_t n = state.maxDemand.size();
    result.need = computeNeed(state);
    std::vector<int> work = state.available;
    std::vector<bool> finished(n, false);

    bool progress = true;
    while (progress) {
        progress = false;
        for (std::size_t i = 0; i < n; ++i) {  // one pass in index order
            if (finished[i] || !lessOrEqual(result.need[i], work)) continue;
            SafetyStep step;
            step.process = static_cast<int>(i);
            step.need = result.need[i];
            step.workBefore = work;
            for (std::size_t j = 0; j < work.size(); ++j) work[j] += state.allocation[i][j];
            step.workAfter = work;
            result.steps.push_back(step);
            result.sequence.push_back(static_cast<int>(i));
            finished[i] = true;
            progress = true;
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!finished[i]) result.unfinished.push_back(static_cast<int>(i));
    }
    result.safe = result.unfinished.empty();
    if (result.safe) {
        result.explanation = "SAFE: every process can finish in the order shown, so no deadlock can be forced.";
    } else {
        result.explanation = "UNSAFE: " + std::to_string(result.unfinished.size()) +
                             " process(es) can never be guaranteed to finish with the resources that would be free.";
    }
    return result;
}

RequestOutcome requestResources(const BankerState& state, int process, const std::vector<int>& request) {
    RequestOutcome out;
    const std::size_t n = state.maxDemand.size();
    const std::size_t m = state.available.size();
    if (process < 0 || static_cast<std::size_t>(process) >= n) {
        out.reason = "unknown process index";
        return out;
    }
    if (request.size() != m) {
        out.reason = "request must have one value per resource type";
        return out;
    }
    for (int v : request) {
        if (v < 0) {
            out.reason = "request values must not be negative";
            return out;
        }
    }
    const IntMatrix need = computeNeed(state);
    const std::size_t p = static_cast<std::size_t>(process);
    if (!lessOrEqual(request, need[p])) {
        out.reason = "DENIED: the process asks for more than its declared maximum claim (request > need)";
        return out;
    }
    if (!lessOrEqual(request, state.available)) {
        out.reason = "WAIT: not enough resources are available right now (request > available)";
        return out;
    }
    BankerState hypothetical = state;  // pretend to grant, then test safety
    for (std::size_t j = 0; j < m; ++j) {
        hypothetical.available[j] -= request[j];
        hypothetical.allocation[p][j] += request[j];
    }
    out.safetyIfGranted = checkSafety(hypothetical);
    out.safetyEvaluated = true;
    if (out.safetyIfGranted.safe) {
        out.granted = true;
        out.reason = "GRANTED: the resulting state is safe";
    } else {
        out.reason = "WAIT: granting would leave the system in an UNSAFE state, so the request is delayed";
    }
    return out;
}

std::vector<std::string> validateDetectionInput(const std::vector<int>& available, const IntMatrix& allocation,
                                                const IntMatrix& request) {
    return shapeErrors(available, allocation, request, "allocation", "request");
}

DetectionResult detectDeadlock(const std::vector<int>& available, const IntMatrix& allocation, const IntMatrix& request) {
    DetectionResult result;
    const std::size_t n = allocation.size();
    std::vector<int> work = available;
    std::vector<bool> finished(n, false);
    // A process holding nothing cannot be part of a deadlock: it finishes without releasing anything.
    for (std::size_t i = 0; i < n; ++i) {
        bool holds = false;
        for (int v : allocation[i]) if (v > 0) holds = true;
        if (!holds) finished[i] = true;
    }
    bool progress = true;
    while (progress) {
        progress = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (finished[i] || !lessOrEqual(request[i], work)) continue;
            for (std::size_t j = 0; j < work.size(); ++j) work[j] += allocation[i][j];
            finished[i] = true;
            result.finishOrder.push_back(static_cast<int>(i));
            progress = true;
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!finished[i]) result.deadlockedProcesses.push_back(static_cast<int>(i));
    }
    result.deadlocked = !result.deadlockedProcesses.empty();
    result.explanation = result.deadlocked
                             ? "DEADLOCK: the listed processes wait for each other's resources and none can proceed."
                             : "No deadlock: every process can eventually finish.";
    return result;
}
