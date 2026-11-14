// Deterministic tests. Every expected value below was calculated BY HAND from
// the rules in the README, not copied from program output.
// Build: see README ("Run the tests").
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "simulation.h"
#include "workload.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failures;                                                                \
            std::cout << "  FAIL line " << __LINE__ << ": " #cond << "\n";               \
        }                                                                                \
    } while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

static JobRequest req(const std::string& name, int cpu, int ram, int sto, int dur,
                      Priority p = Priority::Medium, int arrival = 0) {
    return JobRequest{name, Resources{cpu, ram, sto}, dur, p, arrival};
}

static std::vector<Server> oneServer(int cpu = 4, int ram = 16, int sto = 100) {
    return {Server(1, "S1", Resources{cpu, ram, sto})};
}

static bool logContains(const Simulation& sim, const std::string& text) {
    for (const LogEntry& e : sim.decisionLog()) {
        if (e.message.find(text) != std::string::npos) return true;
    }
    return false;
}

static void testEmptyWorkload() {
    Simulation sim(defaultServers(), Policy::FCFS);
    sim.runToCompletion();
    const Metrics m = sim.metrics();
    CHECK(m.totalJobs == 0 && m.completedJobs == 0);
    CHECK(near(m.avgWaitingTime, 0) && near(m.avgTurnaroundTime, 0) && near(m.throughput, 0));
    CHECK(sim.clock() == 0);
    CHECK(sim.timeline().empty());
}

static void testValidation() {
    Simulation sim(defaultServers(), Policy::FCFS);
    const int zeroCpu = sim.submit(req("a", 0, 1, 1, 1));
    const int negRam = sim.submit(req("b", 1, -4, 1, 1));
    const int zeroSto = sim.submit(req("c", 1, 1, 0, 1));
    const int zeroDur = sim.submit(req("d", 1, 1, 1, 0));
    const int negArr = sim.submit(req("e", 1, 1, 1, 1, Priority::Low, -1));
    const int noName = sim.submit(req("  ", 1, 1, 1, 1));
    for (int id : {zeroCpu, negRam, zeroSto, zeroDur, negArr, noName}) {
        CHECK(sim.findJob(id)->state == JobState::Rejected);
        CHECK(!sim.findJob(id)->reason.empty());
    }
    // exceeds every server on one dimension
    const int tooBig = sim.submit(req("big", 32, 1, 1, 1));
    CHECK(sim.findJob(tooBig)->state == JobState::Rejected);
    // fits only Server-2 (16 cores) -> accepted
    const int onlyS2 = sim.submit(req("s2only", 16, 1, 1, 1));
    CHECK(sim.findJob(onlyS2)->state == JobState::Queued);
    // each dimension fits SOME server but no single server fits all -> reject
    std::vector<Server> servers = {Server(1, "A", Resources{16, 8, 100}), Server(2, "B", Resources{4, 64, 100})};
    Simulation mixed(servers, Policy::FCFS);
    const int split = mixed.submit(req("split", 8, 32, 10, 1));
    CHECK(mixed.findJob(split)->state == JobState::Rejected);
    // rejected jobs never reserve anything and never enter metrics
    sim.runToCompletion();
    CHECK(sim.metrics().rejectedJobs == 7);
    CHECK(sim.metrics().completedJobs == 1);
    for (const Server& s : sim.resources().servers()) {
        CHECK(s.available().cpu == s.total().cpu);
    }
}

static void testStateTransitions() {
    CHECK(isValidTransition(JobState::Queued, JobState::Running));
    CHECK(isValidTransition(JobState::Queued, JobState::Waiting));
    CHECK(isValidTransition(JobState::Waiting, JobState::Running));
    CHECK(isValidTransition(JobState::Running, JobState::Completed));
    CHECK(isValidTransition(JobState::Queued, JobState::Rejected));
    CHECK(isValidTransition(JobState::Waiting, JobState::Rejected));
    CHECK(!isValidTransition(JobState::Queued, JobState::Completed));
    CHECK(!isValidTransition(JobState::Running, JobState::Waiting));
    CHECK(!isValidTransition(JobState::Running, JobState::Rejected));
    CHECK(!isValidTransition(JobState::Completed, JobState::Running));
    CHECK(!isValidTransition(JobState::Rejected, JobState::Queued));
    Job j;
    CHECK(!j.transitionTo(JobState::Completed));
    CHECK(j.state == JobState::Queued);
    CHECK(j.transitionTo(JobState::Running));
    CHECK(j.transitionTo(JobState::Completed));
    CHECK(!j.transitionTo(JobState::Running));
}

// Three jobs on a 4-core server (each needs all 4 cores, so they run one by one).
// A: Low, dur 5 | B: Medium, dur 2 | C: High, dur 1 | all arrive at t=0.
static void buildSerial(Simulation& sim) {
    sim.submit(req("A", 4, 1, 1, 5, Priority::Low));
    sim.submit(req("B", 4, 1, 1, 2, Priority::Medium));
    sim.submit(req("C", 4, 1, 1, 1, Priority::High));
}

static void testFcfsOrderAndMetrics() {
    Simulation sim(oneServer(), Policy::FCFS);
    buildSerial(sim);
    sim.runToCompletion();
    // FCFS ignores priority: A 0-5, B 5-7, C 7-8
    CHECK(sim.findJob(1)->startTime == 0 && sim.findJob(1)->completionTime == 5);
    CHECK(sim.findJob(2)->startTime == 5 && sim.findJob(2)->completionTime == 7);
    CHECK(sim.findJob(3)->startTime == 7 && sim.findJob(3)->completionTime == 8);
    const Metrics m = sim.metrics();
    CHECK(near(m.avgWaitingTime, 4.0));            // (0+5+7)/3
    CHECK(near(m.avgTurnaroundTime, 20.0 / 3.0));  // (5+7+8)/3
    CHECK(m.simulationDuration == 8 && near(m.throughput, 3.0 / 8.0));
    CHECK(m.completedJobs == 3);
}

static void testPriorityOrderAndMetrics() {
    Simulation sim(oneServer(), Policy::Priority);
    buildSerial(sim);
    sim.runToCompletion();
    // High first: C 0-1, then Medium B 1-3, then Low A 3-8
    CHECK(sim.findJob(3)->startTime == 0 && sim.findJob(3)->completionTime == 1);
    CHECK(sim.findJob(2)->startTime == 1 && sim.findJob(2)->completionTime == 3);
    CHECK(sim.findJob(1)->startTime == 3 && sim.findJob(1)->completionTime == 8);
    const Metrics m = sim.metrics();
    CHECK(near(m.avgWaitingTime, 4.0 / 3.0));  // (0+1+3)/3
    CHECK(near(m.avgTurnaroundTime, 4.0));     // (1+3+8)/3
    CHECK(m.simulationDuration == 8 && near(m.throughput, 0.375));
}

static void testEqualPrioritiesUseSubmissionOrder() {
    Simulation sim(oneServer(), Policy::Priority);
    sim.submit(req("first", 4, 1, 1, 1));
    sim.submit(req("second", 4, 1, 1, 1));
    sim.submit(req("third", 4, 1, 1, 1));
    sim.runToCompletion();
    CHECK(sim.findJob(1)->startTime == 0);
    CHECK(sim.findJob(2)->startTime == 1);
    CHECK(sim.findJob(3)->startTime == 2);
}

static void testNonPreemptive() {
    Simulation sim(oneServer(), Policy::Priority);
    sim.submit(req("low", 4, 1, 1, 5, Priority::Low, 0));
    sim.submit(req("high", 4, 1, 1, 1, Priority::High, 1));
    sim.runToCompletion();
    CHECK(sim.findJob(1)->startTime == 0 && sim.findJob(1)->completionTime == 5);  // never interrupted
    CHECK(sim.findJob(2)->startTime == 5 && sim.findJob(2)->completionTime == 6);
    CHECK(sim.findJob(2)->waitingTime() == 4);
}

static void testLeastLoadedPlacement() {
    // Equal servers: tie -> lowest ID; then the emptier server wins.
    std::vector<Server> two = {Server(1, "S1", Resources{8, 32, 100}), Server(2, "S2", Resources{8, 32, 100})};
    Simulation sim(two, Policy::FCFS);
    sim.submit(req("j1", 2, 4, 10, 10));
    sim.submit(req("j2", 2, 4, 10, 10));
    sim.submit(req("j3", 2, 4, 10, 10));
    sim.runUntil(0);
    CHECK(sim.findJob(1)->serverId == 1);  // tie -> lowest ID
    CHECK(sim.findJob(2)->serverId == 2);  // S1 now 0.3167 after, S2 0.1583
    CHECK(sim.findJob(3)->serverId == 1);  // equal again -> lowest ID
    CHECK(logContains(sim, "tie with 1 other server(s)"));
    CHECK(logContains(sim, "Selected S2"));
    // Score formula: (2/8 + 4/32 + 10/100)/3 = 0.15833...
    CHECK(near(Server(9, "x", Resources{8, 32, 100}).loadScoreAfter(Resources{2, 4, 10}), (0.25 + 0.125 + 0.1) / 3.0));

    // Different sizes: the bigger server gets the lower resulting score.
    std::vector<Server> mixed = {Server(1, "Small", Resources{4, 16, 100}), Server(2, "Big", Resources{16, 64, 400})};
    Simulation sim2(mixed, Policy::FCFS);
    sim2.submit(req("x", 2, 4, 10, 5));
    sim2.runUntil(0);
    CHECK(sim2.findJob(1)->serverId == 2);  // Small 0.2833 vs Big 0.0708
}

static void testInsufficientResourcesWaitAndRetry() {
    Simulation sim(oneServer(4, 8, 100), Policy::FCFS);
    sim.submit(req("J1", 3, 4, 10, 4));
    sim.submit(req("J2", 3, 4, 10, 2));  // 3+3 cores > 4
    sim.runUntil(0);
    CHECK(sim.findJob(1)->state == JobState::Running);
    CHECK(sim.findJob(2)->state == JobState::Waiting);
    CHECK(sim.findJob(2)->reason.find("CPU") != std::string::npos);
    CHECK(sim.resources().servers()[0].available().cpu == 1);
    CHECK(logContains(sim, "not eligible"));
    CHECK(logContains(sim, "WAITING"));
    sim.runUntil(3);
    CHECK(sim.findJob(2)->state == JobState::Waiting);
    sim.runUntil(4);  // J1 ends at 4 -> resources released -> J2 retried
    CHECK(sim.findJob(1)->state == JobState::Completed);
    CHECK(sim.findJob(2)->state == JobState::Running && sim.findJob(2)->startTime == 4);
    CHECK(logContains(sim, "started on retry"));
    sim.runToCompletion();
    CHECK(sim.findJob(2)->completionTime == 6);
    CHECK(sim.resources().servers()[0].available().cpu == 4);  // everything released
    const Metrics m = sim.metrics();
    CHECK(near(m.avgWaitingTime, 2.0));      // (0+4)/2
    CHECK(near(m.avgTurnaroundTime, 5.0));   // (4+6)/2
    CHECK(m.simulationDuration == 6 && near(m.throughput, 2.0 / 6.0));
}

static void testMultipleWaitingJobs() {
    Simulation sim(oneServer(4, 16, 100), Policy::FCFS);
    sim.submit(req("J1", 4, 1, 1, 3));
    sim.submit(req("J2", 2, 1, 1, 2));
    sim.submit(req("J3", 2, 1, 1, 2));
    sim.submit(req("J4", 4, 1, 1, 1));
    sim.runUntil(0);
    CHECK(sim.countInState(JobState::Running) == 1);
    CHECK(sim.countInState(JobState::Waiting) == 3);
    sim.runUntil(3);  // J1 done; J2 and J3 share the server; J4 still waits
    CHECK(sim.findJob(2)->state == JobState::Running && sim.findJob(3)->state == JobState::Running);
    CHECK(sim.findJob(4)->state == JobState::Waiting);
    CHECK(sim.resources().servers()[0].available().cpu == 0);
    sim.runToCompletion();
    CHECK(sim.findJob(2)->startTime == 3 && sim.findJob(3)->startTime == 3 && sim.findJob(4)->startTime == 5);
    CHECK(sim.findJob(4)->completionTime == 6);
    const Metrics m = sim.metrics();
    CHECK(near(m.avgWaitingTime, 11.0 / 4.0));      // 0+3+3+5
    CHECK(near(m.avgTurnaroundTime, 19.0 / 4.0));   // 3+5+5+6
    CHECK(m.simulationDuration == 6 && near(m.throughput, 4.0 / 6.0));
}

static void testRepeatedCompletionAndExactlyOnceRelease() {
    Simulation sim(oneServer(4, 8, 100), Policy::FCFS);
    sim.submit(req("J1", 3, 4, 10, 4));
    sim.submit(req("J2", 3, 4, 10, 2));
    sim.runUntil(0);
    CHECK(!sim.completeJob(2, 5));   // WAITING job cannot complete
    CHECK(!sim.completeJob(1, 2));   // too early (needs t >= 4)
    CHECK(!sim.completeJob(99, 5));  // unknown job
    CHECK(sim.resources().servers()[0].available().cpu == 1);
    CHECK(sim.completeJob(1, 4));
    CHECK(sim.resources().servers()[0].available().cpu == 4);
    CHECK(!sim.completeJob(1, 4));   // repeated call: ignored
    CHECK(!sim.completeJob(1, 9));
    CHECK(sim.resources().servers()[0].available().cpu == 4);  // not 7!
    CHECK(sim.resources().servers()[0].available().ramGb == 8);

    // Same at the ResourceManager level.
    ResourceManager rm(oneServer());
    Job job;
    job.id = 1;
    job.demand = Resources{2, 2, 2};
    CHECK(!rm.release(job));        // never allocated
    CHECK(rm.allocate(job, 1));
    CHECK(!rm.allocate(job, 1));    // no double reservation
    CHECK(rm.servers()[0].available().cpu == 2);
    CHECK(rm.release(job));
    CHECK(!rm.release(job));        // no double release
    CHECK(rm.servers()[0].available().cpu == 4);
}

static void testServerLimits() {
    Server s(1, "S", Resources{4, 8, 100});
    CHECK(!s.allocate(Resources{5, 1, 1}));          // too much CPU
    CHECK(s.available().cpu == 4);                    // unchanged
    CHECK(!s.allocate(Resources{-1, 1, 1}));
    CHECK(s.allocate(Resources{4, 8, 100}));          // exactly full is fine
    CHECK(!s.allocate(Resources{1, 1, 1}));
    CHECK(s.available().cpu == 0 && s.available().ramGb == 0);
    CHECK(!s.release(Resources{5, 0, 0}));            // would exceed total
    CHECK(s.release(Resources{4, 8, 100}));
    CHECK(!s.release(Resources{1, 0, 0}));            // already full capacity
    bool threw = false;
    try { Server bad(1, "bad", Resources{0, 8, 8}); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

static void testArrivalGap() {
    Simulation sim(oneServer(), Policy::FCFS);
    sim.submit(req("late", 2, 2, 2, 5, Priority::Medium, 10));
    sim.runToCompletion();
    CHECK(sim.findJob(1)->startTime == 10 && sim.findJob(1)->completionTime == 15);
    const Metrics m = sim.metrics();
    CHECK(m.firstArrival == 10 && m.simulationDuration == 5 && near(m.throughput, 0.2));
    CHECK(near(m.avgWaitingTime, 0.0) && near(m.avgTurnaroundTime, 5.0));
}

static void testMetricsOnlyCountCompleted() {
    Simulation sim(oneServer(), Policy::FCFS);
    sim.submit(req("ok", 4, 1, 1, 4));
    sim.submit(req("bad", 0, 1, 1, 4));
    sim.runUntil(2);
    Metrics m = sim.metrics();
    CHECK(m.completedJobs == 0 && m.rejectedJobs == 1 && m.unfinishedJobs == 1);
    CHECK(near(m.avgWaitingTime, 0) && near(m.throughput, 0));
    sim.runToCompletion();
    m = sim.metrics();
    CHECK(m.completedJobs == 1 && m.rejectedJobs == 1 && m.unfinishedJobs == 0);
    CHECK(near(m.avgWaitingTime, 0.0) && near(m.avgTurnaroundTime, 4.0) && near(m.throughput, 0.25));
}

// Invariants checked after every time step for the sample workload.
static void testInvariantsOnSampleWorkload() {
    for (Policy policy : {Policy::FCFS, Policy::Priority}) {
        Simulation sim(defaultServers(), policy);
        for (const JobRequest& r : sampleJobs()) sim.submit(r);
        for (int t = 0; t <= 80; ++t) {
            sim.runUntil(t);
            for (const Server& s : sim.resources().servers()) {
                const Resources a = s.available();
                CHECK(a.cpu >= 0 && a.cpu <= s.total().cpu);
                CHECK(a.ramGb >= 0 && a.ramGb <= s.total().ramGb);
                CHECK(a.storageGb >= 0 && a.storageGb <= s.total().storageGb);
                int cpu = 0, ram = 0, sto = 0;  // what RUNNING jobs hold must equal what is used
                for (const Job& j : sim.jobs()) {
                    if (j.state == JobState::Running && j.serverId == s.id()) {
                        cpu += j.demand.cpu; ram += j.demand.ramGb; sto += j.demand.storageGb;
                    }
                }
                CHECK(cpu == s.used().cpu && ram == s.used().ramGb && sto == s.used().storageGb);
            }
        }
        CHECK(sim.metrics().completedJobs == 9);  // all but the oversized job
        CHECK(sim.metrics().rejectedJobs == 1);
        CHECK(sim.metrics().unfinishedJobs == 0);  // nothing is left waiting forever
        for (const Server& s : sim.resources().servers()) CHECK(s.used().cpu == 0);
        for (const Job& j : sim.jobs()) CHECK(!j.resourcesHeld);
    }
}

// Same workload, same policy -> identical result (repeatable demo).
static void testRepeatable() {
    auto run = [] {
        Simulation sim(defaultServers(), Policy::Priority);
        for (const JobRequest& r : sampleJobs()) sim.submit(r);
        sim.runToCompletion();
        std::string fingerprint;
        for (const Job& j : sim.jobs()) {
            fingerprint += std::to_string(j.serverId) + ":" + std::to_string(j.startTime) + ":" +
                           std::to_string(j.completionTime) + ";";
        }
        return fingerprint;
    };
    CHECK(run() == run());
}

struct TestCase { const char* name; void (*fn)(); };

int main() {
    const TestCase tests[] = {
        {"empty workload", testEmptyWorkload},
        {"input validation and rejection", testValidation},
        {"job state transitions", testStateTransitions},
        {"FCFS order and metrics", testFcfsOrderAndMetrics},
        {"Priority order and metrics", testPriorityOrderAndMetrics},
        {"equal priorities use submission order", testEqualPrioritiesUseSubmissionOrder},
        {"Priority is non-preemptive", testNonPreemptive},
        {"least-loaded placement and tie-break", testLeastLoadedPlacement},
        {"insufficient resources -> waiting -> retry", testInsufficientResourcesWaitAndRetry},
        {"multiple waiting jobs and release", testMultipleWaitingJobs},
        {"repeated completion / exactly-once release", testRepeatedCompletionAndExactlyOnceRelease},
        {"server capacity limits", testServerLimits},
        {"idle gap before first arrival", testArrivalGap},
        {"metrics count completed jobs only", testMetricsOnlyCountCompleted},
        {"resource invariants on sample workload", testInvariantsOnSampleWorkload},
        {"repeatable results", testRepeatable},
    };
    int failedTests = 0;
    for (const TestCase& t : tests) {
        const int before = g_failures;
        t.fn();
        const bool ok = g_failures == before;
        if (!ok) ++failedTests;
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << t.name << "\n";
    }
    std::cout << "\n" << g_checks << " checks, " << g_failures << " failures, " << failedTests << " failed tests\n";
    return g_failures == 0 ? 0 : 1;
}
