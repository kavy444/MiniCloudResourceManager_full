// main.cpp - console front end: interactive menu and the repeatable demo.
// All simulation logic lives in the other modules; this file only reads input
// and prints.
//
//   mcrm            interactive menu
//   mcrm --demo     repeatable demo: sample workload under FCFS and Priority
//   mcrm --help
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "report.h"
#include "simulation.h"
#include "workload.h"

namespace {

void heading(const std::string& text) {
    std::cout << "\n=== " << text << " ===\n";
}

// ---------- demo mode ----------

void runDemoFor(Policy policy, Metrics& metricsOut) {
    heading("DEMO: " + toString(policy) + " scheduling");
    Simulation sim(defaultServers(), policy);
    for (const JobRequest& r : sampleJobs()) sim.submit(r);

    // Show validation: this request is invalid and ends up REJECTED.
    JobRequest invalid{"invalid-demo-job", Resources{-2, 4, 10}, 3, Priority::Low, 0};
    sim.submit(invalid);

    sim.runUntil(0);
    heading("Cluster state at t=0 (after the first scheduling pass)");
    printServers(std::cout, sim.resources());
    printJobs(std::cout, sim);

    sim.runToCompletion();
    heading("Final job table");
    printJobs(std::cout, sim);
    heading("Execution timeline (Gantt), grouped by server");
    printGantt(std::cout, sim);
    heading("Resource-allocation decision log");
    printDecisionLog(std::cout, sim);
    heading("Metrics");
    printMetrics(std::cout, sim);
    heading("Cluster state at end (everything must be released)");
    printServers(std::cout, sim.resources());
    metricsOut = sim.metrics();
}

int runDemo() {
    Metrics fcfs, prio;
    runDemoFor(Policy::FCFS, fcfs);
    runDemoFor(Policy::Priority, prio);
    heading("Comparison on the same workload (time unit: tu)");
    std::cout << "Policy     Completed  AvgWait  AvgTurnaround  Duration  Throughput\n";
    auto row = [](const char* name, const Metrics& m) {
        std::cout << name << std::string(11 - std::string(name).size(), ' ') << m.completedJobs << "          "
                  << m.avgWaitingTime << "    " << m.avgTurnaroundTime << "         " << m.simulationDuration
                  << "         " << m.throughput << "\n";
    };
    std::cout.setf(std::ios::fixed);
    std::cout.precision(3);
    row("FCFS", fcfs);
    row("Priority", prio);
    return 0;
}

// ---------- interactive mode ----------

bool readLine(const std::string& prompt, std::string& out) {
    std::cout << prompt;
    return static_cast<bool>(std::getline(std::cin, out));
}

// Parses a whole-number string strictly. Returns false on garbage like "4x".
bool parseInt(const std::string& text, int& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (*end != '\0' || value > 1000000 || value < -1000000) return false;
    out = static_cast<int>(value);
    return true;
}

bool askInt(const std::string& prompt, int& out) {
    std::string line;
    if (!readLine(prompt, line)) return false;
    if (!parseInt(line, out)) {
        std::cout << "  '" << line << "' is not a valid whole number. Cancelled.\n";
        return false;
    }
    return true;
}

void submitJobInteractively(Simulation& sim) {
    JobRequest r;
    std::string text;
    if (!readLine("Job name: ", r.name)) return;
    if (!askInt("CPU cores: ", r.demand.cpu)) return;
    if (!askInt("RAM (GB): ", r.demand.ramGb)) return;
    if (!askInt("Storage (GB): ", r.demand.storageGb)) return;
    if (!askInt("Duration (time units): ", r.duration)) return;
    if (!readLine("Priority (High/Medium/Low or 1/2/3): ", text)) return;
    if (!parsePriority(text, r.priority)) {
        std::cout << "  Unknown priority. Cancelled.\n";
        return;
    }
    std::cout << "Arrival time [Enter = now, t=" << sim.clock() << "]: ";
    std::getline(std::cin, text);
    r.arrivalTime = sim.clock();
    if (!text.empty() && !parseInt(text, r.arrivalTime)) {
        std::cout << "  Not a valid whole number. Cancelled.\n";
        return;
    }
    const int id = sim.submit(r);
    const Job* job = sim.findJob(id);
    std::cout << "  Job " << id << " -> " << toString(job->state);
    if (!job->reason.empty()) std::cout << "  (" << job->reason << ")";
    std::cout << "\n";
}

std::vector<Server> configureServersInteractively() {
    std::vector<Server> servers;
    for (int i = 1; i <= 3; ++i) {
        Resources total;
        std::cout << "Server-" << i << ":\n";
        if (!askInt("  CPU cores: ", total.cpu) || !askInt("  RAM (GB): ", total.ramGb) ||
            !askInt("  Storage (GB): ", total.storageGb)) {
            return {};
        }
        try {
            servers.emplace_back(i, "Server-" + std::to_string(i), total);
        } catch (const std::invalid_argument& e) {
            std::cout << "  " << e.what() << " Configuration cancelled.\n";
            return {};
        }
    }
    return servers;
}

int runInteractive() {
    std::vector<Server> servers = defaultServers();
    Policy policy = Policy::FCFS;
    auto sim = std::make_unique<Simulation>(servers, policy);

    std::cout << "Mini Cloud Resource Manager - console simulator\n"
                 "(a simulation: nothing here touches a real cloud)\n";
    while (true) {
        std::cout << "\n[Policy: " << toString(policy) << " | clock t=" << sim->clock() << " | jobs: "
                  << sim->jobs().size() << "]\n"
                     " 1) Show servers          6) Run simulation to completion\n"
                     " 2) Submit a job          7) Show decision log\n"
                     " 3) Load sample jobs      8) Show metrics and timeline\n"
                     " 4) Show jobs             9) Configure servers (resets)\n"
                     " 5) Choose algorithm     10) Reset simulation\n"
                     " 0) Exit\n";
        std::string choice;
        if (!readLine("Choose: ", choice)) break;
        int option = -1;
        if (!parseInt(choice, option)) option = -1;

        switch (option) {
            case 0: return 0;
            case 1: printServers(std::cout, sim->resources()); break;
            case 2: submitJobInteractively(*sim); break;
            case 3:
                for (const JobRequest& r : sampleJobs()) {
                    JobRequest copy = r;
                    copy.arrivalTime += sim->clock();  // relative to "now"
                    sim->submit(copy);
                }
                std::cout << "  Loaded " << sampleJobs().size() << " sample jobs.\n";
                break;
            case 4: printJobs(std::cout, *sim); break;
            case 5: {
                std::string text;
                readLine("Algorithm (1 = FCFS, 2 = Priority): ", text);
                Policy chosen;
                if (!parsePolicy(text, chosen)) {
                    std::cout << "  Unknown algorithm.\n";
                } else if (!sim->setPolicy(chosen)) {
                    std::cout << "  The simulation has already run; use option 10 to reset first.\n";
                } else {
                    policy = chosen;
                    std::cout << "  Algorithm set to " << toString(policy) << ".\n";
                }
                break;
            }
            case 6:
                sim->runToCompletion();
                std::cout << "  Simulation finished at t=" << sim->clock() << ".\n";
                printJobs(std::cout, *sim);
                break;
            case 7: printDecisionLog(std::cout, *sim); break;
            case 8:
                printGantt(std::cout, *sim);
                std::cout << "\n";
                printMetrics(std::cout, *sim);
                break;
            case 9: {
                std::vector<Server> fresh = configureServersInteractively();
                if (!fresh.empty()) {
                    servers = fresh;
                    sim = std::make_unique<Simulation>(servers, policy);
                    std::cout << "  Servers configured; simulation reset.\n";
                }
                break;
            }
            case 10:
                sim = std::make_unique<Simulation>(servers, policy);
                std::cout << "  Simulation reset.\n";
                break;
            default: std::cout << "  Please enter a number from the menu.\n";
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        const std::string arg = argv[1];
        if (arg == "--demo") return runDemo();
        std::cout << "Usage: mcrm [--demo]\n  (no argument) interactive menu\n  --demo        repeatable sample workload\n";
        return arg == "--help" ? 0 : 1;
    }
    return runInteractive();
}
