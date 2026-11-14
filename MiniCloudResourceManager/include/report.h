// report.h - console formatting only. Reads simulation state, never changes it.
#pragma once

#include <ostream>

#include "simulation.h"

void printServers(std::ostream& os, const ResourceManager& resources);
void printJobs(std::ostream& os, const Simulation& sim);
void printDecisionLog(std::ostream& os, const Simulation& sim);
void printGantt(std::ostream& os, const Simulation& sim);
void printMetrics(std::ostream& os, const Simulation& sim);
