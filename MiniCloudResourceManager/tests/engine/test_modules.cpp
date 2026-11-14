// Tests for the modules around the CPU scheduler: JSON, ranking, Banker's algorithm, memory management,
// synchronization demos and the JSON engine protocol. Expected values are derived by hand (comments show how).
#include "banker.h"
#include "cpu_scheduler.h"
#include "engine_api.h"
#include "json.h"
#include "memory_manager.h"
#include "ranking.h"
#include "sync_demo.h"
#include "test_framework.h"

namespace {

Json parseOrDie(const std::string& text) {
    Json j;
    std::string error;
    const bool ok = Json::parse(text, j, error);
    CHECK(ok);
    return j;
}

Json call(const std::string& request) { return parseOrDie(handleRequestText(request)); }

bool okField(const Json& j) {
    const Json* v = j.find("ok");
    return v != nullptr && v->isBool() && v->asBool();
}

}  // namespace

// ------------------------------------------------------------------ JSON
TEST(json_round_trip_and_escaping) {
    Json j = parseOrDie(R"({"a":[1,2.5,"x\n\"y\"",true,null],"b":{"c":-7}})");
    CHECK(j.find("a") != nullptr && j.find("a")->size() == 5);
    CHECK(j.find("a")->items()[0].isInt());
    CHECK_NEAR(j.find("a")->items()[1].asDouble(), 2.5);
    CHECK(j.find("a")->items()[2].asString() == "x\n\"y\"");
    CHECK(j.find("b")->find("c")->asInt() == -7);
    Json again = parseOrDie(j.dump());           // dump -> parse gives the same document
    CHECK(again.dump() == j.dump());
}

TEST(json_rejects_malformed_input) {
    Json j;
    std::string error;
    for (const char* bad : {"", "{", "{\"a\":}", "[1,]", "{\"a\":1} extra", "nul", "\"unterminated", "{'a':1}"}) {
        CHECK(!Json::parse(bad, j, error));
        CHECK(!error.empty());
    }
    // deeply nested input must be refused, not crash the engine
    std::string deep(5000, '[');
    CHECK(!Json::parse(deep, j, error));
}

// ------------------------------------------------------------------ ranking
TEST(ranking_lists_every_tied_winner_and_ignores_empty_runs) {
    ScheduleMetrics a, b, empty;
    a.completed = b.completed = 3;
    a.avgWaiting = 4.0; b.avgWaiting = 2.0;
    a.throughput = b.throughput = 0.5;
    empty.completed = 0;  // zero metrics must not "win"
    const auto best = rankMetrics({{"A", a}, {"B", b}, {"EMPTY", empty}});
    bool sawWait = false, sawThroughput = false;
    for (const BestForMetric& m : best) {
        if (m.metric == "avg_waiting") {
            sawWait = true;
            CHECK(m.lowerIsBetter);
            CHECK(m.labels.size() == 1 && m.labels[0] == "B");
            CHECK(!m.tie);
        }
        if (m.metric == "throughput") {
            sawThroughput = true;
            CHECK(!m.lowerIsBetter);
            CHECK(m.labels.size() == 2 && m.tie && m.allEqual);
        }
    }
    CHECK(sawWait && sawThroughput);
}

// ------------------------------------------------------------------ Banker's algorithm
// Classic textbook state: 5 processes, 3 resource types, available (3,3,2). Safe sequence <P1,P3,P4,P0,P2>.
static BankerState textbookState() {
    BankerState s;
    s.available = {3, 3, 2};
    s.maxDemand = {{7, 5, 3}, {3, 2, 2}, {9, 0, 2}, {2, 2, 2}, {4, 3, 3}};
    s.allocation = {{0, 1, 0}, {2, 0, 0}, {3, 0, 2}, {2, 1, 1}, {0, 0, 2}};
    return s;
}

TEST(banker_textbook_state_is_safe_with_known_sequence) {
    const SafetyResult r = checkSafety(textbookState());
    CHECK(r.safe);
    CHECK((r.sequence == std::vector<int>{1, 3, 4, 0, 2}));
    CHECK((r.need[0] == std::vector<int>{7, 4, 3}));   // max - allocation
    CHECK((r.need[1] == std::vector<int>{1, 2, 2}));
    CHECK(r.unfinished.empty());
}

TEST(banker_request_granted_only_when_safe) {
    const BankerState s = textbookState();
    // P1 asks (1,0,2): <= need (1,2,2), <= available (3,3,2); resulting state is safe -> granted.
    RequestOutcome ok = requestResources(s, 1, {1, 0, 2});
    CHECK(ok.granted && ok.safetyEvaluated && ok.safetyIfGranted.safe);
    // P4 asks (3,3,0): available allows it, but then work=(0,0,2) and nobody's need fits -> unsafe -> must wait.
    RequestOutcome unsafe = requestResources(s, 4, {3, 3, 0});
    CHECK(!unsafe.granted && unsafe.safetyEvaluated && !unsafe.safetyIfGranted.safe);
    // P0 asks (0,2,0): safe (verified by hand with the safety algorithm) -> granted.
    CHECK(requestResources(s, 0, {0, 2, 0}).granted);
    // More than its remaining need -> refused without even running the safety check.
    RequestOutcome tooMuch = requestResources(s, 1, {2, 0, 0});
    CHECK(!tooMuch.granted && !tooMuch.safetyEvaluated);
    // More than what is available -> must wait.
    RequestOutcome notFree = requestResources(s, 2, {6, 0, 0});
    CHECK(!notFree.granted && !notFree.safetyEvaluated);
    // The input state itself is never modified by a request.
    CHECK((s.available == std::vector<int>{3, 3, 2}));
}

TEST(banker_unsafe_state_and_validation) {
    BankerState s;
    s.available = {0};
    s.maxDemand = {{2}, {2}};
    s.allocation = {{1}, {1}};  // each holds 1, needs 1 more, nothing free
    const SafetyResult r = checkSafety(s);
    CHECK(!r.safe);
    CHECK(r.sequence.empty());
    CHECK((r.unfinished == std::vector<int>{0, 1}));

    BankerState bad = textbookState();
    bad.allocation[0][0] = 99;  // allocation > max
    CHECK(!validateBankerState(bad).empty());
    BankerState neg = textbookState();
    neg.available[1] = -1;
    CHECK(!validateBankerState(neg).empty());
    BankerState ragged = textbookState();
    ragged.maxDemand[2].pop_back();
    CHECK(!validateBankerState(ragged).empty());
}

TEST(deadlock_detection_finds_circular_wait) {
    // P0 holds R0 and wants R1; P1 holds R1 and wants R0; nothing free -> both deadlocked.
    const DetectionResult d = detectDeadlock({0, 0}, {{1, 0}, {0, 1}}, {{0, 1}, {1, 0}});
    CHECK(d.deadlocked);
    CHECK((d.deadlockedProcesses == std::vector<int>{0, 1}));
    // One free instance of R1 lets P0 finish, which frees R0 for P1 -> no deadlock.
    const DetectionResult fine = detectDeadlock({0, 1}, {{1, 0}, {0, 1}}, {{0, 1}, {1, 0}});
    CHECK(!fine.deadlocked);
    CHECK((fine.finishOrder == std::vector<int>{0, 1}));
}

// ------------------------------------------------------------------ memory management
// Fixed partitions 100,500,200,300,600 start at addresses 0,100,600,800,1100. Requests 212,417,112,426.
static std::vector<MemoryOp> fourAllocs() {
    return {{"ALLOC", "P1", 212}, {"ALLOC", "P2", 417}, {"ALLOC", "P3", 112}, {"ALLOC", "P4", 426}};
}

static MemoryResult runFixed(FitStrategy strategy) {
    MemoryConfig cfg;
    cfg.strategy = strategy;
    cfg.partitions = {100, 500, 200, 300, 600};
    return simulateMemory(cfg, fourAllocs());
}

TEST(memory_first_fit_fixed_partitions) {
    const MemoryResult r = runFixed(FitStrategy::FIRST_FIT);
    CHECK(r.ok && r.steps.size() == 4);
    CHECK(r.steps[0].blockStart == 100);   // 212 -> first block >= 212 is 500
    CHECK(r.steps[1].blockStart == 1100);  // 417 -> 500 is taken, next is 600
    CHECK(r.steps[2].blockStart == 600);   // 112 -> 200
    CHECK(!r.steps[3].success);            // 426 -> nothing left big enough
    CHECK(r.failedAllocations == 1);
}

TEST(memory_best_fit_fixed_partitions) {
    const MemoryResult r = runFixed(FitStrategy::BEST_FIT);
    CHECK(r.steps[0].blockStart == 800);   // 212 -> smallest block >= 212 is 300
    CHECK(r.steps[1].blockStart == 100);   // 417 -> 500
    CHECK(r.steps[2].blockStart == 600);   // 112 -> 200
    CHECK(r.steps[3].blockStart == 1100);  // 426 -> 600
    CHECK(r.failedAllocations == 0);
    // internal fragmentation = (300-212)+(500-417)+(200-112)+(600-426) = 88+83+88+174
    CHECK(r.finalStats.internalFragmentation == 433);
}

TEST(memory_worst_fit_fixed_partitions) {
    const MemoryResult r = runFixed(FitStrategy::WORST_FIT);
    CHECK(r.steps[0].blockStart == 1100);  // 212 -> largest block is 600
    CHECK(r.steps[1].blockStart == 100);   // 417 -> largest remaining is 500
    CHECK(r.steps[2].blockStart == 800);   // 112 -> largest remaining is 300
    CHECK(!r.steps[3].success);            // 426 -> remaining 100 and 200 are too small
}

TEST(memory_dynamic_split_free_and_coalesce) {
    MemoryConfig cfg;
    cfg.strategy = FitStrategy::FIRST_FIT;
    cfg.totalSize = 100;
    const MemoryResult r = simulateMemory(cfg, {{"ALLOC", "A", 30}, {"ALLOC", "B", 30}, {"ALLOC", "C", 30},
                                                {"FREE", "A", 0}, {"FREE", "C", 0}, {"FREE", "B", 0}});
    CHECK(r.ok && r.steps.size() == 6);
    CHECK(r.steps[2].stats.free == 10);                      // 100 - 90
    CHECK(r.steps[3].stats.freeBlocks == 2);                 // hole at 0..30 and tail 90..100
    CHECK(r.steps[4].stats.largestFree == 10 + 30);          // C (60..90) merged with the tail -> 40
    CHECK(r.steps[4].stats.externalFragmentation == 30);     // free 70 - largest 40
    CHECK(r.finalStats.freeBlocks == 1 && r.finalStats.largestFree == 100 && r.finalStats.allocated == 0);
}

TEST(memory_best_vs_worst_fit_choose_different_holes) {
    // Holes of 50 (addr 0) and 20 (addr 60) after freeing; a request of 15 -> best fit uses the 20 hole.
    MemoryConfig cfg;
    cfg.totalSize = 100;
    const std::vector<MemoryOp> prep = {{"ALLOC", "A", 50}, {"ALLOC", "B", 10}, {"ALLOC", "C", 20}, {"ALLOC", "D", 20},
                                        {"FREE", "A", 0}, {"FREE", "C", 0}};
    cfg.strategy = FitStrategy::BEST_FIT;
    std::vector<MemoryOp> ops = prep;
    ops.push_back({"ALLOC", "E", 15});
    CHECK(simulateMemory(cfg, ops).steps.back().blockStart == 60);
    cfg.strategy = FitStrategy::WORST_FIT;
    CHECK(simulateMemory(cfg, ops).steps.back().blockStart == 0);
    cfg.strategy = FitStrategy::FIRST_FIT;
    CHECK(simulateMemory(cfg, ops).steps.back().blockStart == 0);
}

TEST(memory_validation_and_failures) {
    MemoryConfig cfg;
    cfg.totalSize = 100;
    CHECK(!simulateMemory(cfg, {{"ALLOC", "A", 0}}).ok);                       // size must be positive
    CHECK(!simulateMemory(cfg, {{"ALLOC", "A", -5}}).ok);
    CHECK(!simulateMemory(cfg, {{"MOVE", "A", 5}}).ok);                        // unknown operation
    MemoryConfig empty;
    CHECK(!simulateMemory(empty, {{"ALLOC", "A", 5}}).ok);                     // no memory configured
    const MemoryResult r = simulateMemory(cfg, {{"ALLOC", "A", 60}, {"ALLOC", "A", 10}, {"FREE", "ZZ", 0}, {"ALLOC", "B", 90}});
    CHECK(r.ok);
    CHECK(!r.steps[1].success);   // duplicate owner name
    CHECK(!r.steps[2].success);   // freeing something that is not allocated
    CHECK(!r.steps[3].success);   // too big for the 40 left
    CHECK(r.failedAllocations == 1);   // only "no space" failures are counted, not duplicate names or bad frees
    CHECK(r.finalStats.allocated == 60);
}

// ------------------------------------------------------------------ synchronization demos
TEST(sync_race_loses_updates_without_mutex_only) {
    const SyncResult bad = raceCondition(4, false);
    CHECK(bad.ok && !bad.deadlock);
    CHECK(bad.expectedCounter == 8);
    CHECK(bad.finalCounter < 8 && bad.lostUpdates == 8 - bad.finalCounter && bad.lostUpdates > 0);
    const SyncResult good = raceCondition(4, true);
    CHECK(good.finalCounter == 8 && good.lostUpdates == 0 && !good.deadlock);
    CHECK(!raceCondition(0, true).ok);
    CHECK(!raceCondition(51, true).ok);
    // deterministic: identical input -> identical trace
    CHECK(raceCondition(4, false).steps.size() == bad.steps.size());
}

TEST(sync_producer_consumer_correct_order) {
    for (int delay : {0, 20}) {
        const SyncResult r = producerConsumer(6, 2, false, delay);
        CHECK(r.ok && !r.deadlock && !r.truncated);
        CHECK(r.produced == 6 && r.consumed == 6 && r.consumedInOrder);
        CHECK(r.maxBufferOccupancy <= 2);
    }
    CHECK(producerConsumer(6, 2, false, 20).maxBufferOccupancy == 2);  // the late consumer let the buffer fill up
}

TEST(sync_wrong_semaphore_order_deadlocks_when_buffer_fills) {
    // Buffer of 2, consumer starts late: the producer fills the buffer (2 items), then on item 3 takes the
    // mutex and blocks on 'empty'. The consumer needs the mutex -> nobody can run.
    const SyncResult r = producerConsumer(6, 2, true, 20);
    CHECK(r.ok && r.deadlock);
    CHECK(r.produced == 2 && r.consumed == 0);
    // Without the late consumer the lock-step schedule drains the buffer, so no deadlock occurs.
    CHECK(!producerConsumer(6, 2, true, 0).deadlock);
    CHECK(!producerConsumer(0, 2, false, 0).ok);
    CHECK(!producerConsumer(6, 0, false, 0).ok);
    CHECK(!producerConsumer(6, 2, false, 201).ok);
}

// ------------------------------------------------------------------ JSON engine protocol
TEST(engine_info_and_unknown_command) {
    const Json info = call(R"({"command":"info"})");
    CHECK(okField(info));
    CHECK(info.find("algorithms")->size() == 9);
    const Json bad = call(R"({"command":"nope"})");
    CHECK(!okField(bad));
    CHECK(bad.find("errors") != nullptr && bad.find("errors")->size() >= 1);
    CHECK(!okField(call("not json")));
    CHECK(!okField(call("[1,2,3]")));
    CHECK(!okField(call("{}")));
}

TEST(engine_schedule_fcfs_matches_hand_calculation) {
    // P1 arr0 burst5, P2 arr1 burst3, P3 arr2 burst1.  FCFS: P1 0-5, P2 5-8, P3 8-9.
    // CT 5,8,9  TAT 5,7,7  WT 0,4,6  RT 0,4,6.  avg WT 10/3, avg TAT 19/3, throughput 3/9.
    const Json r = call(R"({"command":"schedule","algorithm":"FCFS","processes":[
        {"id":1,"name":"P1","arrival":0,"burst":5},{"id":2,"name":"P2","arrival":1,"burst":3},
        {"id":3,"name":"P3","arrival":2,"burst":1}]})");
    CHECK(okField(r));
    const Json& m = *r.find("metrics");
    CHECK_NEAR(m.find("avg_waiting")->asDouble(), 10.0 / 3.0);
    CHECK_NEAR(m.find("avg_turnaround")->asDouble(), 19.0 / 3.0);
    CHECK_NEAR(m.find("avg_response")->asDouble(), 10.0 / 3.0);
    CHECK_NEAR(m.find("throughput")->asDouble(), 3.0 / 9.0);
    CHECK_NEAR(m.find("cpu_utilization")->asDouble(), 100.0);
    CHECK(m.find("context_switches")->asInt() == 2);   // P1->P2, P2->P3
    const auto& segs = r.find("segments")->items();
    CHECK(segs.size() == 3);
    CHECK(segs[0].find("start")->asInt() == 0 && segs[0].find("end")->asInt() == 5);
    CHECK(segs[2].find("process_id")->asInt() == 3 && segs[2].find("end")->asInt() == 9);
}

TEST(engine_schedule_reports_idle_and_validation_errors) {
    // P1 0..2, gap 2..5 (idle), P2 5..6 -> utilization 3/6 = 50%.
    const Json r = call(R"({"command":"schedule","algorithm":"FCFS","processes":[
        {"id":1,"arrival":0,"burst":2},{"id":2,"arrival":5,"burst":1}]})");
    CHECK(okField(r));
    CHECK_NEAR(r.find("metrics")->find("cpu_utilization")->asDouble(), 50.0);
    bool idle = false;
    for (const Json& s : r.find("segments")->items()) {
        if (s.find("type")->asString() == "IDLE") idle = s.find("start")->asInt() == 2 && s.find("end")->asInt() == 5;
    }
    CHECK(idle);
    CHECK(!okField(call(R"({"command":"schedule","algorithm":"FCFS","processes":[{"id":1,"arrival":0,"burst":0}]})")));
    CHECK(!okField(call(R"({"command":"schedule","algorithm":"FCFS","processes":[{"id":1,"arrival":-1,"burst":2}]})")));
    CHECK(!okField(call(R"({"command":"schedule","algorithm":"FCFS","processes":[{"id":1,"arrival":0,"burst":2},{"id":1,"arrival":0,"burst":2}]})")));
    CHECK(!okField(call(R"({"command":"schedule","algorithm":"BOGUS","processes":[{"id":1,"arrival":0,"burst":2}]})")));
    CHECK(!okField(call(R"({"command":"schedule","algorithm":"RR","config":{"time_quantum":0},"processes":[{"id":1,"arrival":0,"burst":2}]})")));
    CHECK(!okField(call(R"({"command":"schedule","algorithm":"FCFS","processes":[{"id":"x","arrival":0,"burst":2}]})")));
    // Empty input is a clean, explained failure, not a crash.
    const Json empty = call(R"({"command":"schedule","algorithm":"FCFS","processes":[]})");
    CHECK(!okField(empty) || empty.find("metrics")->find("completed")->asInt() == 0);
}

TEST(engine_compare_runs_every_algorithm_on_the_same_input) {
    const Json r = call(R"({"command":"compare","algorithms":["FCFS","SJF","SRTF","RR"],"config":{"time_quantum":2},
        "processes":[{"id":1,"arrival":0,"burst":7},{"id":2,"arrival":2,"burst":4},{"id":3,"arrival":4,"burst":1},{"id":4,"arrival":5,"burst":4}]})");
    CHECK(okField(r));
    CHECK(r.find("results")->size() == 4);
    CHECK(r.find("best") != nullptr && r.find("best")->size() >= 6);
    // SRTF minimizes average waiting time on this classic example (it is provably optimal for waiting time).
    for (const Json& b : r.find("best")->items()) {
        if (b.find("metric")->asString() == "avg_waiting") {
            CHECK(b.find("best")->size() >= 1 && b.find("best")->items()[0].asString() == "SRTF");
        }
    }
    CHECK(!okField(call(R"({"command":"compare","algorithms":["FCFS","FCFS"],"processes":[{"id":1,"arrival":0,"burst":1}]})")));
    CHECK(!okField(call(R"({"command":"compare","algorithms":[],"processes":[{"id":1,"arrival":0,"burst":1}]})")));
}

TEST(engine_place_explains_every_server) {
    // S1 8/32/500 empty, S2 16/64/1000 with 8 cores used. Job 4/8/50 -> scores:
    // S1 (4/8 + 8/32 + 50/500)/3 = 0.2833   S2 ((8+4)/16 + 8/64 + 50/1000)/3 = 0.3083  -> S1 wins.
    const Json r = call(R"({"command":"place","servers":[
        {"id":1,"cpu":8,"ram_gb":32,"storage_gb":500},
        {"id":2,"cpu":16,"ram_gb":64,"storage_gb":1000,"used_cpu":8}],
        "job":{"cpu":4,"ram_gb":8,"storage_gb":50}})");
    CHECK(okField(r));
    CHECK(r.find("found")->asBool() && r.find("server_id")->asInt() == 1);
    CHECK(r.find("evaluations")->size() == 2);
    CHECK_NEAR(r.find("evaluations")->items()[0].find("score")->asDouble(), (0.5 + 0.25 + 0.1) / 3.0);
    CHECK_NEAR(r.find("evaluations")->items()[1].find("score")->asDouble(), (0.75 + 0.125 + 0.05) / 3.0);
    // Over-capacity: nothing fits and it can never fit.
    const Json big = call(R"({"command":"place","servers":[{"id":1,"cpu":8,"ram_gb":32,"storage_gb":500}],"job":{"cpu":64,"ram_gb":1,"storage_gb":1}})");
    CHECK(okField(big) && !big.find("found")->asBool() && !big.find("fits_some_total")->asBool());
    // 'used' larger than the server total is refused.
    CHECK(!okField(call(R"({"command":"place","servers":[{"id":1,"cpu":8,"ram_gb":32,"storage_gb":500,"used_cpu":9}],"job":{"cpu":1,"ram_gb":1,"storage_gb":1}})")));
}

TEST(engine_banker_memory_sync_commands) {
    const Json s = call(R"({"command":"banker","action":"safety","available":[3,3,2],
        "max":[[7,5,3],[3,2,2],[9,0,2],[2,2,2],[4,3,3]],"allocation":[[0,1,0],[2,0,0],[3,0,2],[2,1,1],[0,0,2]]})");
    CHECK(okField(s) && s.find("safety")->find("safe")->asBool());
    CHECK(s.find("safety")->find("sequence")->size() == 5);
    CHECK(!okField(call(R"({"command":"banker","action":"safety","available":[1],"max":[[1,1]],"allocation":[[0,0]]})")));
    const Json m = call(R"({"command":"memory","strategy":"BEST_FIT","partitions":[100,500,200,300,600],
        "operations":[{"type":"ALLOC","process":"P1","size":212},{"type":"ALLOC","process":"P2","size":417}]})");
    CHECK(okField(m) && m.find("steps")->size() == 2);
    CHECK(m.find("steps")->items()[0].find("block_start")->asInt() == 800);
    CHECK(!okField(call(R"({"command":"memory","strategy":"NEXT_FIT","total_size":10,"operations":[]})")));
    const Json y = call(R"({"command":"sync","scenario":"producer_consumer","items":6,"buffer_size":2,"wrong_order":true,"consumer_delay":20})");
    CHECK(okField(y) && y.find("deadlock")->asBool());
    CHECK(!okField(call(R"({"command":"sync","scenario":"nope"})")));
}
