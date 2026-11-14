#include "workload.h"

std::vector<Server> defaultServers() {
    return {Server(1, "Server-1", Resources{8, 32, 500}),
            Server(2, "Server-2", Resources{16, 64, 1000}),
            Server(3, "Server-3", Resources{4, 16, 250})};
}

std::vector<JobRequest> sampleJobs() {
    // name, {cpu, ram GB, storage GB}, duration, priority, arrival
    return {
        {"web-frontend", {2, 4, 20}, 6, Priority::Medium, 0},
        {"db-backup", {4, 8, 300}, 10, Priority::Low, 0},
        {"ml-training", {12, 48, 400}, 12, Priority::High, 0},
        {"video-encode", {8, 16, 200}, 8, Priority::Medium, 0},
        {"log-analytics", {6, 24, 150}, 5, Priority::Low, 1},
        {"api-gateway", {2, 2, 10}, 4, Priority::High, 2},
        {"batch-report", {4, 12, 100}, 7, Priority::Medium, 3},
        {"cache-warmup", {1, 4, 5}, 3, Priority::High, 4},
        {"mega-simulation", {32, 128, 100}, 5, Priority::Medium, 4},
        {"data-migration", {4, 8, 900}, 9, Priority::Low, 5},
    };
}
