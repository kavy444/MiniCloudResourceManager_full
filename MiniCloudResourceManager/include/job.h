// job.h - Job data structure, priority, state machine and input validation.
#pragma once

#include <string>

#include "resources.h"

// Priority ordering used by Priority Scheduling:  High > Medium > Low.
enum class Priority { High, Medium, Low };

// Job life cycle. Allowed transitions (enforced by Job::transitionTo):
//   QUEUED  -> RUNNING | WAITING | REJECTED
//   WAITING -> RUNNING | REJECTED
//   RUNNING -> COMPLETED
// COMPLETED and REJECTED are final.
//
// QUEUED  = submitted, not yet examined by a scheduling pass.
// WAITING = examined, job is valid, but no server can fit it *right now*.
enum class JobState { Queued, Running, Waiting, Completed, Rejected };

// Smaller rank = more urgent (High=0, Medium=1, Low=2).
int priorityRank(Priority p);
std::string toString(Priority p);
std::string toString(JobState s);

// Accepts "high"/"h"/"1", "medium"/"m"/"2", "low"/"l"/"3" (case-insensitive).
bool parsePriority(const std::string& text, Priority& out);

bool isValidTransition(JobState from, JobState to);

// What a user submits. Raw values, not yet validated.
struct JobRequest {
    std::string name;
    Resources demand;
    int duration = 0;            // burst time, in simulation time units
    Priority priority = Priority::Medium;
    int arrivalTime = 0;         // simulation time at which the job arrives
};

// Returns an empty string when the request is valid, otherwise a readable
// message listing every problem (zero/negative values, empty name, ...).
// Capacity checks against the servers live in ResourceManager.
std::string validateJobRequest(const JobRequest& request);

struct Job {
    int id = 0;                  // submission order, starts at 1
    std::string name;
    Resources demand;
    int duration = 0;
    Priority priority = Priority::Medium;
    int arrivalTime = 0;

    JobState state = JobState::Queued;
    int serverId = -1;           // server it runs/ran on, -1 if never placed
    bool resourcesHeld = false;  // true only while resources are reserved
    int startTime = -1;
    int completionTime = -1;
    std::string reason;          // why WAITING / REJECTED (human readable)

    // Moves to `next` only if the transition is legal. Returns success.
    bool transitionTo(JobState next);

    // waiting = start - arrival; -1 if the job never started.
    int waitingTime() const;
    // turnaround = completion - arrival; -1 if the job has not completed.
    int turnaroundTime() const;
};
