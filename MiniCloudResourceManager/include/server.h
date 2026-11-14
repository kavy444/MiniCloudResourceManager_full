// server.h - one simulated server with total and available CPU/RAM/storage.
#pragma once

#include <string>

#include "resources.h"

class Server {
public:
    // Throws std::invalid_argument if any capacity is not positive.
    Server(int id, std::string name, Resources total);

    int id() const { return id_; }
    const std::string& name() const { return name_; }
    const Resources& total() const { return total_; }
    const Resources& available() const { return available_; }
    Resources used() const;

    // True only if ALL three requested resources fit in what is available now.
    bool canFit(const Resources& request) const;

    // Empty string if the request fits, otherwise e.g.
    // "CPU (needs 8, free 4), RAM (needs 16 GB, free 8 GB)".
    std::string shortfall(const Resources& request) const;

    // Reserves resources. Returns false and changes nothing if it doesn't fit
    // or the request has a negative component.
    bool allocate(const Resources& request);

    // Gives resources back. Returns false and changes nothing if it would push
    // available above total (i.e. releasing something that was never reserved).
    bool release(const Resources& request);

    // Utilization ratios in [0, 1].
    double cpuUtilization() const;
    double ramUtilization() const;
    double storageUtilization() const;
    double overallUtilization() const;  // average of the three

    // LOAD SCORE (lower is better): the average of the CPU, RAM and storage
    // utilization ratios this server WOULD have after placing `request`:
    //   ((usedCpu+reqCpu)/totalCpu + (usedRam+reqRam)/totalRam +
    //    (usedStorage+reqStorage)/totalStorage) / 3
    double loadScoreAfter(const Resources& request) const;

private:
    int id_;
    std::string name_;
    Resources total_;
    Resources available_;
};
