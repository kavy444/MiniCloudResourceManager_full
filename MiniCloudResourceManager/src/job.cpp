#include "job.h"

#include <cctype>
#include <vector>

int priorityRank(Priority p) {
    switch (p) {
        case Priority::High: return 0;
        case Priority::Medium: return 1;
        case Priority::Low: return 2;
    }
    return 1;
}

std::string toString(Priority p) {
    switch (p) {
        case Priority::High: return "High";
        case Priority::Medium: return "Medium";
        case Priority::Low: return "Low";
    }
    return "Medium";
}

std::string toString(JobState s) {
    switch (s) {
        case JobState::Queued: return "QUEUED";
        case JobState::Running: return "RUNNING";
        case JobState::Waiting: return "WAITING";
        case JobState::Completed: return "COMPLETED";
        case JobState::Rejected: return "REJECTED";
    }
    return "UNKNOWN";
}

bool parsePriority(const std::string& text, Priority& out) {
    std::string t;
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (t == "high" || t == "h" || t == "1") { out = Priority::High; return true; }
    if (t == "medium" || t == "m" || t == "2") { out = Priority::Medium; return true; }
    if (t == "low" || t == "l" || t == "3") { out = Priority::Low; return true; }
    return false;
}

bool isValidTransition(JobState from, JobState to) {
    switch (from) {
        case JobState::Queued:
            return to == JobState::Running || to == JobState::Waiting || to == JobState::Rejected;
        case JobState::Waiting:
            return to == JobState::Running || to == JobState::Rejected;
        case JobState::Running:
            return to == JobState::Completed;
        case JobState::Completed:
        case JobState::Rejected:
            return false;
    }
    return false;
}

std::string validateJobRequest(const JobRequest& r) {
    std::vector<std::string> problems;

    bool nameBlank = true;
    for (char c : r.name) {
        if (!std::isspace(static_cast<unsigned char>(c))) { nameBlank = false; break; }
    }
    if (nameBlank) problems.push_back("job name must not be empty");
    if (r.demand.cpu <= 0) problems.push_back("CPU request must be at least 1 core (got " + std::to_string(r.demand.cpu) + ")");
    if (r.demand.ramGb <= 0) problems.push_back("RAM request must be at least 1 GB (got " + std::to_string(r.demand.ramGb) + ")");
    if (r.demand.storageGb <= 0) problems.push_back("storage request must be at least 1 GB (got " + std::to_string(r.demand.storageGb) + ")");
    if (r.duration <= 0) problems.push_back("duration must be at least 1 time unit (got " + std::to_string(r.duration) + ")");
    if (r.arrivalTime < 0) problems.push_back("arrival time must not be negative (got " + std::to_string(r.arrivalTime) + ")");

    std::string message;
    for (std::size_t i = 0; i < problems.size(); ++i) {
        if (i > 0) message += "; ";
        message += problems[i];
    }
    return message;
}

bool Job::transitionTo(JobState next) {
    if (!isValidTransition(state, next)) return false;
    state = next;
    return true;
}

int Job::waitingTime() const {
    return startTime < 0 ? -1 : startTime - arrivalTime;
}

int Job::turnaroundTime() const {
    return completionTime < 0 ? -1 : completionTime - arrivalTime;
}
