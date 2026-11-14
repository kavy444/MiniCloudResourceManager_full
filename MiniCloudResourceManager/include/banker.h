// banker.h - deadlock AVOIDANCE (Banker's algorithm) and deadlock DETECTION for multi-instance resources.
//
// Notation (n processes, m resource types):
//   available[m]      free instances right now
//   maxDemand[n][m]   the most each process may ever ask for
//   allocation[n][m]  what each process holds now
//   need = maxDemand - allocation
//
// SAFETY ALGORITHM: work = available. Repeat full passes over the processes in index order; a process that
// is unfinished and has need <= work "finishes" and returns its allocation (work += allocation). The state is
// SAFE if all processes can finish; the order they finished in is the safe sequence. (Scanning in passes
// reproduces the textbook example <P1,P3,P4,P0,P2>.)
//
// RESOURCE-REQUEST ALGORITHM: a request is granted only if request <= need, request <= available AND the
// state that would result is still safe. Otherwise the process must wait (nothing is changed).
//
// DETECTION: same idea but uses the CURRENT outstanding requests instead of max-need; any process that can
// never finish is deadlocked.
//
// This is a teaching simulation; it does not control any real resources.
#pragma once

#include <string>
#include <vector>

using IntMatrix = std::vector<std::vector<int>>;

struct BankerState {
    std::vector<std::string> processNames;   // optional; defaults to P0, P1, ...
    std::vector<std::string> resourceNames;  // optional; defaults to R0, R1, ...
    std::vector<int> available;
    IntMatrix maxDemand;
    IntMatrix allocation;
};

struct SafetyStep {
    int process = 0;
    std::vector<int> need;
    std::vector<int> workBefore;
    std::vector<int> workAfter;
};

struct SafetyResult {
    bool safe = false;
    std::vector<int> sequence;      // process indices in the order they can finish
    std::vector<SafetyStep> steps;
    IntMatrix need;
    std::vector<int> unfinished;    // when unsafe: processes that can never finish
    std::string explanation;
};

struct RequestOutcome {
    bool granted = false;
    std::string reason;
    SafetyResult safetyIfGranted;   // safety check of the hypothetical state (only when it was evaluated)
    bool safetyEvaluated = false;
};

struct DetectionResult {
    bool deadlocked = false;
    std::vector<int> deadlockedProcesses;
    std::vector<int> finishOrder;
    std::string explanation;
};

std::vector<std::string> validateBankerState(const BankerState& state);

SafetyResult checkSafety(const BankerState& state);

RequestOutcome requestResources(const BankerState& state, int process, const std::vector<int>& request);

// `request[i][j]` = instances of resource j that process i is currently blocked waiting for.
DetectionResult detectDeadlock(const std::vector<int>& available, const IntMatrix& allocation, const IntMatrix& request);

std::vector<std::string> validateDetectionInput(const std::vector<int>& available, const IntMatrix& allocation,
                                                const IntMatrix& request);
