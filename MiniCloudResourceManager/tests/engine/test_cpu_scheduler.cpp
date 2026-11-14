// Tests for the CPU scheduling engine. Every expected number was worked out BY HAND (see comments)
// before the code was run.
#include <algorithm>
#include <map>
#include <tuple>

#include "cpu_scheduler.h"
#include "test_framework.h"

namespace {

using Seg = std::tuple<int, int, int>;  // process id, start, end

SchedProcess P(int id, int arrival, int burst, int priority = 2, int queue = -1) {
    SchedProcess p;
    p.id = id;
    p.name = "P" + std::to_string(id);
    p.arrival = arrival;
    p.burst = burst;
    p.priority = priority;
    p.queue = queue;
    p.demand = Resources{1, 1, 1};
    return p;
}

std::vector<Seg> execSegs(const ScheduleResult& r, int server = 1) {
    std::vector<Seg> out;
    for (const Segment& s : r.segments) {
        if (s.type == "EXEC" && s.serverId == server) out.emplace_back(s.processId, s.start, s.end);
    }
    return out;
}

const ProcessResult& pr(const ScheduleResult& r, int id) {
    for (const ProcessResult& p : r.processes) if (p.id == id) return p;
    static ProcessResult none;
    return none;
}

bool hasEvent(const ScheduleResult& r, const std::string& type, int pid, int time) {
    for (const EngineEvent& e : r.events) {
        if (e.type == type && e.processId == pid && e.time == time) return true;
    }
    return false;
}

// Classic workload used by several tests: arrival/burst  P1 0/5, P2 1/3, P3 2/8, P4 3/6
std::vector<SchedProcess> classic(int p1 = 2, int p2 = 2, int p3 = 2, int p4 = 2) {
    return {P(1, 0, 5, p1), P(2, 1, 3, p2), P(3, 2, 8, p3), P(4, 3, 6, p4)};
}

void expectTimes(const ScheduleResult& r, std::vector<int> ct, std::vector<int> tat, std::vector<int> wt,
                 std::vector<int> rt) {
    for (std::size_t i = 0; i < ct.size(); ++i) {
        const ProcessResult& p = pr(r, static_cast<int>(i) + 1);
        CHECK(p.completion == ct[i]);
        CHECK(p.turnaround == tat[i]);
        CHECK(p.waiting == wt[i]);
        CHECK(p.response == rt[i]);
    }
}

}  // namespace

TEST(fcfs_classic) {
    // P1 0-5, P2 5-8, P3 8-16, P4 16-22
    const ScheduleResult r = runSchedule(classic(), Algorithm::FCFS, SchedConfig{});
    CHECK(r.ok);
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 5}, {2, 5, 8}, {3, 8, 16}, {4, 16, 22}}));
    expectTimes(r, {5, 8, 16, 22}, {5, 7, 14, 19}, {0, 4, 6, 13}, {0, 4, 6, 13});
    CHECK_NEAR(r.metrics.avgTurnaround, 45.0 / 4);  // 11.25
    CHECK_NEAR(r.metrics.avgWaiting, 23.0 / 4);     // 5.75
    CHECK_NEAR(r.metrics.avgResponse, 23.0 / 4);
    CHECK(r.metrics.totalTime == 22);
    CHECK_NEAR(r.metrics.throughput, 4.0 / 22);
    CHECK_NEAR(r.metrics.cpuUtilization, 100.0);
    CHECK(r.metrics.contextSwitches == 3);
    CHECK(r.metrics.preemptions == 0);
    CHECK(r.segments.front().endReason == "COMPLETED");
}

TEST(sjf_classic) {
    // P1 0-5, then shortest of {P2:3,P3:8,P4:6}: P2 5-8, P4 8-14, P3 14-22
    const ScheduleResult r = runSchedule(classic(), Algorithm::SJF, SchedConfig{});
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 5}, {2, 5, 8}, {4, 8, 14}, {3, 14, 22}}));
    expectTimes(r, {5, 8, 22, 14}, {5, 7, 20, 11}, {0, 4, 12, 5}, {0, 4, 12, 5});
    CHECK_NEAR(r.metrics.avgTurnaround, 43.0 / 4);
    CHECK_NEAR(r.metrics.avgWaiting, 21.0 / 4);
}

TEST(sjf_equal_bursts_use_arrival_then_id) {
    // P1 0/5 runs first; P2 and P3 both arrive at 1 with burst 2 -> P2 (lower id) before P3
    const ScheduleResult r = runSchedule({P(1, 0, 5), P(3, 1, 2), P(2, 1, 2)}, Algorithm::SJF, SchedConfig{});
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 5}, {2, 5, 7}, {3, 7, 9}}));
}

TEST(priority_np_classic) {
    // priorities P1:2 P2:4 P3:1 P4:3 (smaller = higher). P1 0-5; then P3(1) 5-13, P4(3) 13-19, P2(4) 19-22
    const ScheduleResult r = runSchedule(classic(2, 4, 1, 3), Algorithm::PRIORITY_NP, SchedConfig{});
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 5}, {3, 5, 13}, {4, 13, 19}, {2, 19, 22}}));
    expectTimes(r, {5, 22, 13, 19}, {5, 21, 11, 16}, {0, 18, 3, 10}, {0, 18, 3, 10});
    CHECK_NEAR(r.metrics.avgTurnaround, 53.0 / 4);
    CHECK_NEAR(r.metrics.avgWaiting, 31.0 / 4);
    CHECK(r.metrics.preemptions == 0);
}

TEST(priority_np_equal_priorities_fcfs_tiebreak) {
    const ScheduleResult r = runSchedule({P(1, 0, 3, 2), P(2, 0, 2, 2), P(3, 0, 1, 2)}, Algorithm::PRIORITY_NP, SchedConfig{});
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 3}, {2, 3, 5}, {3, 5, 6}}));
}

TEST(hrrn_prefers_long_waiting_job) {
    // P1 0/6, P2 1/5, P3 5/2. At t=6: ratio P2 = (5+5)/5 = 2.0, P3 = (1+2)/2 = 1.5 -> P2 first (SJF would pick P3)
    const std::vector<SchedProcess> procs = {P(1, 0, 6), P(2, 1, 5), P(3, 5, 2)};
    const ScheduleResult h = runSchedule(procs, Algorithm::HRRN, SchedConfig{});
    CHECK((execSegs(h) == std::vector<Seg>{{1, 0, 6}, {2, 6, 11}, {3, 11, 13}}));
    expectTimes(h, {6, 11, 13}, {6, 10, 8}, {0, 5, 6}, {0, 5, 6});
    const ScheduleResult s = runSchedule(procs, Algorithm::SJF, SchedConfig{});
    CHECK((execSegs(s) == std::vector<Seg>{{1, 0, 6}, {3, 6, 8}, {2, 8, 13}}));
}

TEST(srtf_classic_preempts) {
    // P1 0-1 (preempted by P2: 3 < 4 left), P2 1-4, P1 4-8, P4 8-14, P3 14-22
    const ScheduleResult r = runSchedule(classic(), Algorithm::SRTF, SchedConfig{});
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 1}, {2, 1, 4}, {1, 4, 8}, {4, 8, 14}, {3, 14, 22}}));
    expectTimes(r, {8, 4, 22, 14}, {8, 3, 20, 11}, {3, 0, 12, 5}, {0, 0, 12, 5});
    CHECK_NEAR(r.metrics.avgTurnaround, 42.0 / 4);  // 10.5
    CHECK_NEAR(r.metrics.avgWaiting, 20.0 / 4);     // 5.0
    CHECK_NEAR(r.metrics.avgResponse, 17.0 / 4);    // 4.25
    CHECK(r.metrics.contextSwitches == 4);
    CHECK(r.metrics.preemptions == 1);
    CHECK(r.segments[0].endReason == "PREEMPTED");
    CHECK(hasEvent(r, "JOB_PREEMPTED", 1, 1));
}

TEST(srtf_equal_remaining_does_not_preempt) {
    // P1 0/4 running, P2 arrives at 2 with burst 2 == P1's remaining 2 -> P1 keeps the CPU
    const ScheduleResult r = runSchedule({P(1, 0, 4), P(2, 2, 2)}, Algorithm::SRTF, SchedConfig{});
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 4}, {2, 4, 6}}));
    CHECK(r.metrics.preemptions == 0);
}

TEST(round_robin_classic_q2) {
    // Queue order rule: an arrival at the instant a quantum expires enters the queue BEFORE the expired process.
    SchedConfig cfg;
    cfg.timeQuantum = 2;
    const ScheduleResult r = runSchedule(classic(), Algorithm::RR, cfg);
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 2}, {2, 2, 4}, {3, 4, 6}, {1, 6, 8}, {4, 8, 10}, {2, 10, 11},
                                           {3, 11, 13}, {1, 13, 14}, {4, 14, 16}, {3, 16, 18}, {4, 18, 20}, {3, 20, 22}}));
    expectTimes(r, {14, 11, 22, 20}, {14, 10, 20, 17}, {9, 7, 12, 11}, {0, 1, 2, 5});
    CHECK_NEAR(r.metrics.avgTurnaround, 61.0 / 4);  // 15.25
    CHECK_NEAR(r.metrics.avgWaiting, 39.0 / 4);     // 9.75
    CHECK_NEAR(r.metrics.avgResponse, 2.0);
    CHECK(r.metrics.contextSwitches == 11);
    CHECK(r.metrics.preemptions == 8);
}

TEST(round_robin_quantum_boundaries) {
    SchedConfig cfg;
    cfg.timeQuantum = 2;
    // burst == quantum: finishes exactly at the boundary, one COMPLETED segment, no preemption
    ScheduleResult r = runSchedule({P(1, 0, 2)}, Algorithm::RR, cfg);
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 2}}));
    CHECK(r.segments[0].endReason == "COMPLETED");
    CHECK(r.metrics.preemptions == 0);
    // burst < quantum
    r = runSchedule({P(1, 0, 1)}, Algorithm::RR, cfg);
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 1}}));
    // single long process: expiries are merged into one segment, no context switch
    r = runSchedule({P(1, 0, 5)}, Algorithm::RR, cfg);
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 5}}));
    CHECK(r.metrics.contextSwitches == 0);
    CHECK(r.metrics.preemptions == 2);  // expired at 2 and 4, immediately re-selected
    // quantum larger than every burst behaves like FCFS
    cfg.timeQuantum = 100;
    r = runSchedule(classic(), Algorithm::RR, cfg);
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 5}, {2, 5, 8}, {3, 8, 16}, {4, 16, 22}}));
}

TEST(priority_p_classic) {
    // priorities P1:2 P2:4 P3:1 P4:3. P1 0-2, P3 (1) preempts at 2 and runs 2-10, P1 10-13, P4 13-19, P2 19-22
    const ScheduleResult r = runSchedule(classic(2, 4, 1, 3), Algorithm::PRIORITY_P, SchedConfig{});
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 2}, {3, 2, 10}, {1, 10, 13}, {4, 13, 19}, {2, 19, 22}}));
    expectTimes(r, {13, 22, 10, 19}, {13, 21, 8, 16}, {8, 18, 0, 10}, {0, 18, 0, 10});
    CHECK_NEAR(r.metrics.avgTurnaround, 58.0 / 4);  // 14.5
    CHECK_NEAR(r.metrics.avgWaiting, 36.0 / 4);     // 9.0
    CHECK_NEAR(r.metrics.avgResponse, 7.0);
    CHECK(r.metrics.contextSwitches == 4);
    CHECK(r.metrics.preemptions == 1);
}

TEST(priority_p_equal_priority_does_not_preempt) {
    const ScheduleResult r = runSchedule({P(1, 0, 4, 3), P(2, 1, 1, 3)}, Algorithm::PRIORITY_P, SchedConfig{});
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 4}, {2, 4, 5}}));
}

TEST(mlq_higher_queue_preempts_lower) {
    // Q0: RR q=2, Q1: FCFS. P1 0/6 in Q1, P2 1/3 in Q0, P3 2/2 in Q1.
    // P1 0-1 (preempted by P2), P2 1-3 (quantum), P2 3-4 (done; merged into 1-4), P1 4-9, P3 9-11
    SchedConfig cfg;
    cfg.mlq = {{QueuePolicy::RR, 2}, {QueuePolicy::FCFS, 0}};
    const ScheduleResult r = runSchedule({P(1, 0, 6, 2, 1), P(2, 1, 3, 1, 0), P(3, 2, 2, 2, 1)}, Algorithm::MLQ, cfg);
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 1}, {2, 1, 4}, {1, 4, 9}, {3, 9, 11}}));
    expectTimes(r, {9, 4, 11}, {9, 3, 9}, {3, 0, 7}, {0, 0, 7});
    CHECK_NEAR(r.metrics.avgTurnaround, 7.0);
    CHECK(r.metrics.contextSwitches == 3);
    CHECK(r.metrics.preemptions == 2);  // P1 preempted at 1; P2's quantum expired at 3 (re-selected immediately)
    CHECK(r.segments[0].queueLevel == 1);
    CHECK(r.segments[1].queueLevel == 0);
}

TEST(mlq_automatic_queue_from_priority) {
    SchedProcess a = P(1, 0, 1, 1), b = P(2, 0, 1, 3), c = P(3, 0, 1, 9), d = P(4, 0, 1, 0);
    CHECK(mlqQueueOf(a, 3) == 0);
    CHECK(mlqQueueOf(b, 3) == 2);
    CHECK(mlqQueueOf(c, 3) == 2);  // clamped to the last queue
    CHECK(mlqQueueOf(d, 3) == 0);  // priority 0 -> queue 0
    SchedProcess e = P(5, 0, 1, 1, 1);
    CHECK(mlqQueueOf(e, 3) == 1);  // explicit queue wins
}

TEST(mlq_inner_queue_policies) {
    // One queue using SJF: behaves like non-preemptive SJF
    SchedConfig cfg;
    cfg.mlq = {{QueuePolicy::SJF, 0}};
    ScheduleResult r = runSchedule(classic(), Algorithm::MLQ, cfg);
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 5}, {2, 5, 8}, {4, 8, 14}, {3, 14, 22}}));
    // One queue using PRIORITY: non-preemptive priority
    cfg.mlq = {{QueuePolicy::PRIORITY, 0}};
    r = runSchedule(classic(2, 4, 1, 3), Algorithm::MLQ, cfg);
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 5}, {3, 5, 13}, {4, 13, 19}, {2, 19, 22}}));
}

TEST(mlfq_demotion_sequence) {
    // quanta {2,4,8}. P1 0/10, P2 1/3.
    // P1 0-2 (q0, demoted), P2 2-4 (q0, demoted), P1 4-8 (q1, demoted), P2 8-9 (done), P1 9-13 (q2, done)
    SchedConfig cfg;
    cfg.mlfqQuanta = {2, 4, 8};
    const ScheduleResult r = runSchedule({P(1, 0, 10), P(2, 1, 3)}, Algorithm::MLFQ, cfg);
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 2}, {2, 2, 4}, {1, 4, 8}, {2, 8, 9}, {1, 9, 13}}));
    expectTimes(r, {13, 9}, {13, 8}, {3, 5}, {0, 1});
    CHECK_NEAR(r.metrics.avgTurnaround, 10.5);
    CHECK_NEAR(r.metrics.avgWaiting, 4.0);
    CHECK_NEAR(r.metrics.avgResponse, 0.5);
    CHECK(r.metrics.contextSwitches == 4);
    CHECK(r.segments[0].queueLevel == 0);
    CHECK(r.segments[2].queueLevel == 1);
    CHECK(r.segments[4].queueLevel == 2);
    CHECK(hasEvent(r, "JOB_DEMOTED", 1, 2));
    CHECK(hasEvent(r, "JOB_DEMOTED", 2, 4));
    CHECK(hasEvent(r, "JOB_DEMOTED", 1, 8));
}

TEST(mlfq_new_arrival_preempts_lower_queue) {
    // quanta {4,8}. P1 0/10: 0-4 in q0, demoted, 4-5 in q1 until P2 (5/2, q0) preempts; P2 5-7; P1 7-12
    SchedConfig cfg;
    cfg.mlfqQuanta = {4, 8};
    const ScheduleResult r = runSchedule({P(1, 0, 10), P(2, 5, 2)}, Algorithm::MLFQ, cfg);
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 4}, {1, 4, 5}, {2, 5, 7}, {1, 7, 12}}));
    CHECK(r.metrics.contextSwitches == 2);  // P1->P2, P2->P1 (P1->P1 is not a switch)
    CHECK(r.segments[1].endReason == "PREEMPTED");
    CHECK(pr(r, 1).completion == 12);
}

TEST(mlfq_aging_promotes_waiting_process) {
    // quanta {2,100}. P1 0/4, P2 2/2, P3 4/2, P4 6/2. P1 is demoted at t=2 and waits.
    const std::vector<SchedProcess> procs = {P(1, 0, 4), P(2, 2, 2), P(3, 4, 2), P(4, 6, 2)};
    SchedConfig cfg;
    cfg.mlfqQuanta = {2, 100};
    ScheduleResult r = runSchedule(procs, Algorithm::MLFQ, cfg);  // no aging: P1 starves until the end
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 2}, {2, 2, 4}, {3, 4, 6}, {4, 6, 8}, {1, 8, 10}}));
    cfg.agingThreshold = 3;  // P1 waits from t=2, promoted at t=5, runs before P4
    r = runSchedule(procs, Algorithm::MLFQ, cfg);
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 2}, {2, 2, 4}, {3, 4, 6}, {1, 6, 8}, {4, 8, 10}}));
    CHECK(hasEvent(r, "JOB_PROMOTED", 1, 5));
    CHECK(pr(r, 1).completion == 8);
}

TEST(cpu_idle_periods_and_single_process) {
    // P1 2/3 then P2 10/2: idle 5-10. Clock starts at the first arrival (t=2): total = 12 - 2 = 10
    ScheduleResult r = runSchedule({P(1, 2, 3), P(2, 10, 2)}, Algorithm::FCFS, SchedConfig{});
    CHECK(r.segments.size() == 3);
    CHECK(r.segments[1].type == "IDLE" && r.segments[1].start == 5 && r.segments[1].end == 10);
    CHECK(r.metrics.startTime == 2 && r.metrics.totalTime == 10);
    CHECK_NEAR(r.metrics.cpuUtilization, 50.0);  // busy 5 / 10
    CHECK_NEAR(r.metrics.throughput, 0.2);
    CHECK(r.metrics.contextSwitches == 1);  // P1 -> idle -> P2 counts as one switch
    // single process: no idle, no switch, full utilization, WT = RT = 0
    r = runSchedule({P(7, 3, 4)}, Algorithm::SRTF, SchedConfig{});
    CHECK(r.segments.size() == 1);
    CHECK(pr(r, 7).completion == 7 && pr(r, 7).waiting == 0 && pr(r, 7).response == 0);
    CHECK_NEAR(r.metrics.cpuUtilization, 100.0);
    CHECK(r.metrics.contextSwitches == 0);
}

TEST(simultaneous_arrivals_are_ordered_by_id) {
    for (Algorithm a : {Algorithm::FCFS, Algorithm::SJF, Algorithm::PRIORITY_NP, Algorithm::HRRN}) {
        const ScheduleResult r = runSchedule({P(3, 0, 2), P(1, 0, 2), P(2, 0, 2)}, a, SchedConfig{});
        CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 2}, {2, 2, 4}, {3, 4, 6}}));
    }
}

TEST(validation_rejects_bad_input) {
    SchedConfig cfg;
    CHECK(!runSchedule({}, Algorithm::FCFS, cfg).ok);                          // empty input
    CHECK(!runSchedule({P(1, 0, 0)}, Algorithm::FCFS, cfg).ok);               // zero burst
    CHECK(!runSchedule({P(1, 0, -3)}, Algorithm::FCFS, cfg).ok);              // negative burst
    CHECK(!runSchedule({P(1, -1, 3)}, Algorithm::FCFS, cfg).ok);              // negative arrival
    CHECK(!runSchedule({P(1, 0, 3, -2)}, Algorithm::FCFS, cfg).ok);           // negative priority
    CHECK(!runSchedule({P(1, 0, 3), P(1, 1, 2)}, Algorithm::FCFS, cfg).ok);   // duplicate id
    CHECK(!runSchedule({P(0, 0, 3)}, Algorithm::FCFS, cfg).ok);               // id < 1
    SchedProcess bad = P(1, 0, 3);
    bad.demand.ramGb = 0;
    CHECK(!runSchedule({bad}, Algorithm::FCFS, cfg).ok);                       // zero RAM
    cfg.timeQuantum = 0;
    CHECK(!runSchedule({P(1, 0, 3)}, Algorithm::RR, cfg).ok);                  // quantum 0
    cfg = SchedConfig{};
    cfg.mlfqQuanta = {};
    CHECK(!runSchedule({P(1, 0, 3)}, Algorithm::MLFQ, cfg).ok);
    cfg = SchedConfig{};
    CHECK(!runSchedule({P(1, 0, 3, 2, 9)}, Algorithm::MLQ, cfg).ok);           // queue index out of range
    const ScheduleResult r = runSchedule({P(1, 0, 0)}, Algorithm::FCFS, SchedConfig{});
    CHECK(!r.errors.empty());
    CHECK(r.errors[0].find("burst") != std::string::npos);
}

TEST(every_algorithm_is_deterministic_and_consistent) {
    // Pseudo-random workloads from a fixed seed; check invariants for every algorithm.
    unsigned state = 12345;
    auto next = [&state](int mod) {
        state = state * 1103515245u + 12345u;
        return static_cast<int>((state >> 16) % static_cast<unsigned>(mod));
    };
    for (int round = 0; round < 25; ++round) {
        std::vector<SchedProcess> procs;
        const int n = 1 + next(8);
        for (int i = 1; i <= n; ++i) procs.push_back(P(i, next(12), 1 + next(9), 1 + next(4), next(3) == 0 ? next(3) : -1));
        for (Algorithm a : allAlgorithms()) {
            SchedConfig cfg;
            cfg.timeQuantum = 1 + next(4);
            cfg.agingThreshold = next(2) * (2 + next(5));
            const ScheduleResult r = runSchedule(procs, a, cfg);
            const ScheduleResult again = runSchedule(procs, a, cfg);
            CHECK(r.ok);
            CHECK(execSegs(r) == execSegs(again));  // deterministic
            // every process ran exactly its burst
            std::map<int, int> executed;
            int busy = 0;
            int prevEnd = -1;
            for (const Segment& s : r.segments) {
                CHECK(s.end > s.start);
                CHECK(s.start >= prevEnd);          // one CPU: segments never overlap
                prevEnd = s.end;
                if (s.type == "EXEC") {
                    executed[s.processId] += s.end - s.start;
                    busy += s.end - s.start;
                }
            }
            int sumBursts = 0;
            for (const SchedProcess& p : procs) {
                CHECK(executed[p.id] == p.burst);
                sumBursts += p.burst;
                const ProcessResult& res = pr(r, p.id);
                CHECK(res.status == "COMPLETED");
                CHECK(res.firstStart >= p.arrival);
                CHECK(res.completion >= p.arrival + p.burst);
                CHECK(res.turnaround == res.completion - p.arrival);
                CHECK(res.waiting == res.turnaround - p.burst);
                CHECK(res.waiting >= 0 && res.response >= 0);
                CHECK(res.response <= res.waiting);
            }
            CHECK(busy == sumBursts);
            CHECK(r.metrics.totalTime >= sumBursts);
            CHECK(r.metrics.cpuUtilization <= 100.0 + 1e-9);
        }
    }
}

TEST(cluster_waiting_and_retry_on_release) {
    // One server {4 cores, 8 GB, 100 GB}. P1 and P2 each need 3 cores: P2 must wait until P1 releases at t=4.
    SchedConfig cfg;
    cfg.useResources = true;
    cfg.servers = {Server(1, "S1", Resources{4, 8, 100})};
    SchedProcess a = P(1, 0, 4), b = P(2, 0, 2);
    a.demand = b.demand = Resources{3, 4, 10};
    const ScheduleResult r = runSchedule({a, b}, Algorithm::RR, cfg);
    CHECK(r.ok);
    CHECK((execSegs(r) == std::vector<Seg>{{1, 0, 4}, {2, 4, 6}}));
    CHECK(pr(r, 2).admittedAt == 4);
    CHECK(pr(r, 2).waiting == 4);  // TAT 6 - burst 2
    CHECK(hasEvent(r, "INSUFFICIENT_RESOURCES", 2, 0));
    CHECK(hasEvent(r, "RESOURCES_ALLOCATED", 2, 4));
    CHECK(hasEvent(r, "RESOURCES_RELEASED", 1, 4));
    CHECK(r.allocations.size() == 2);
    CHECK(r.allocations[0].allocatedAt == 0 && r.allocations[0].releasedAt == 4);
    CHECK(r.allocations[1].allocatedAt == 4 && r.allocations[1].releasedAt == 6);
    bool mentionsCpu = false;
    for (const LogEntry& e : r.decisionLog) if (e.message.find("short on CPU") != std::string::npos) mentionsCpu = true;
    CHECK(mentionsCpu);
}

TEST(cluster_rejects_oversized_and_excludes_it_from_metrics) {
    SchedConfig cfg;
    cfg.useResources = true;
    cfg.servers = {Server(1, "S1", Resources{4, 8, 100})};
    SchedProcess big = P(2, 0, 3);
    big.demand = Resources{64, 1, 1};
    const ScheduleResult r = runSchedule({P(1, 0, 3), big}, Algorithm::FCFS, cfg);
    CHECK(r.ok);
    CHECK(pr(r, 2).status == "REJECTED");
    CHECK(pr(r, 2).reason.find("exceeds the total capacity") != std::string::npos);
    CHECK(r.metrics.completed == 1 && r.metrics.rejected == 1);
    CHECK_NEAR(r.metrics.avgTurnaround, 3.0);
    CHECK(hasEvent(r, "JOB_REJECTED", 2, 0));
}

TEST(cluster_places_jobs_on_two_servers_in_parallel) {
    SchedConfig cfg;
    cfg.useResources = true;
    cfg.servers = {Server(1, "S1", Resources{4, 8, 100}), Server(2, "S2", Resources{4, 8, 100})};
    SchedProcess a = P(1, 0, 4), b = P(2, 0, 4);
    a.demand = b.demand = Resources{3, 4, 10};
    const ScheduleResult r = runSchedule({a, b}, Algorithm::FCFS, cfg);
    CHECK((execSegs(r, 1) == std::vector<Seg>{{1, 0, 4}}));  // tie -> lowest server id
    CHECK((execSegs(r, 2) == std::vector<Seg>{{2, 0, 4}}));  // S1 cannot fit a second 3-core job
    CHECK(r.metrics.totalTime == 4);
    CHECK_NEAR(r.metrics.cpuUtilization, 100.0);              // 2 servers busy 4 of 4
    CHECK(pr(r, 1).serverId == 1 && pr(r, 2).serverId == 2);
    CHECK_NEAR(r.serverSummaries[0].avgCpuUtil, 75.0);        // 3 of 4 cores for the whole run
    CHECK(r.serverSummaries[0].peakUsed.cpu == 3);
}

TEST(cluster_capacity_never_exceeded_on_random_workloads) {
    unsigned state = 777;
    auto next = [&state](int mod) {
        state = state * 1103515245u + 12345u;
        return static_cast<int>((state >> 16) % static_cast<unsigned>(mod));
    };
    for (int round = 0; round < 20; ++round) {
        SchedConfig cfg;
        cfg.useResources = true;
        cfg.servers = {Server(1, "A", Resources{8, 32, 500}), Server(2, "B", Resources{4, 16, 250}), Server(3, "C", Resources{16, 64, 1000})};
        std::vector<SchedProcess> procs;
        const int n = 3 + next(10);
        for (int i = 1; i <= n; ++i) {
            SchedProcess p = P(i, next(10), 1 + next(8), 1 + next(3));
            p.demand = Resources{1 + next(9), 1 + next(40), 1 + next(600)};
            procs.push_back(p);
        }
        for (Algorithm a : {Algorithm::FCFS, Algorithm::RR, Algorithm::SRTF, Algorithm::MLFQ}) {
            cfg.timeQuantum = 2;
            const ScheduleResult r = runSchedule(procs, a, cfg);
            CHECK(r.ok);
            // replay the allocation intervals: at no instant may a server's reserved amount exceed its total
            int horizon = 0;
            for (const AllocationRecord& rec : r.allocations) {
                CHECK(rec.releasedAt >= rec.allocatedAt);  // every allocation released
                horizon = std::max(horizon, rec.releasedAt);
            }
            for (int t = 0; t <= horizon; ++t) {
                for (const Server& s : cfg.servers) {
                    Resources used;
                    for (const AllocationRecord& rec : r.allocations) {
                        if (rec.serverId == s.id() && rec.allocatedAt <= t && t < rec.releasedAt) {
                            used.cpu += rec.amount.cpu;
                            used.ramGb += rec.amount.ramGb;
                            used.storageGb += rec.amount.storageGb;
                        }
                    }
                    CHECK(used.cpu <= s.total().cpu && used.ramGb <= s.total().ramGb && used.storageGb <= s.total().storageGb);
                }
            }
            int completed = 0, rejected = 0;
            for (const ProcessResult& p : r.processes) (p.status == "COMPLETED" ? completed : rejected)++;
            CHECK(completed + rejected == n);
            for (const SchedProcess& p : procs) {
                if (pr(r, p.id).status != "COMPLETED") continue;
                int ran = 0;
                for (const Segment& s : r.segments) if (s.type == "EXEC" && s.processId == p.id) ran += s.end - s.start;
                CHECK(ran == p.burst);
            }
            // a single CPU per server never runs two processes at once
            for (const Server& s : cfg.servers) {
                int prevEnd = -1;
                for (const Segment& seg : r.segments) {
                    if (seg.serverId != s.id()) continue;
                    CHECK(seg.start >= prevEnd);
                    prevEnd = seg.end;
                }
            }
        }
    }
}
