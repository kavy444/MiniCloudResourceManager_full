// workload.h - default cluster and the built-in sample jobs (demo data).
#pragma once

#include <vector>

#include "job.h"
#include "server.h"

// Three servers with different sizes (CPU cores / RAM GB / storage GB).
std::vector<Server> defaultServers();

// Ten sample jobs; arrivals 0..5. Includes one job that can never fit.
std::vector<JobRequest> sampleJobs();
