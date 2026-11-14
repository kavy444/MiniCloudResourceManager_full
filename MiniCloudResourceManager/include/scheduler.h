// scheduler.h - CPU-scheduling POLICY: decides the ORDER in which ready jobs
// are considered. It does not choose servers (see resource_manager.h).
//
//   FCFS     : earliest arrival first; ties by submission id.
//   Priority : non-preemptive. High > Medium > Low; ties by earliest arrival,
//              then by submission id (FCFS as tie-breaker).
//
// "Non-preemptive" means a running job is never interrupted, even if a more
// important job arrives.
#pragma once

#include <string>
#include <vector>

#include "job.h"

enum class Policy { FCFS, Priority };

std::string toString(Policy p);
// Accepts "fcfs"/"1" and "priority"/"2" (case-insensitive).
bool parsePolicy(const std::string& text, Policy& out);

class Scheduler {
public:
    explicit Scheduler(Policy policy = Policy::FCFS) : policy_(policy) {}

    Policy policy() const { return policy_; }
    void setPolicy(Policy policy) { policy_ = policy; }

    // True if job `a` must be considered before job `b`. Strict weak ordering
    // (job ids are unique, so no two jobs ever compare equal).
    bool comesBefore(const Job& a, const Job& b) const;

    // Sorts the ready queue in the order jobs should be considered.
    void sortReadyQueue(std::vector<Job*>& ready) const;

private:
    Policy policy_;
};
