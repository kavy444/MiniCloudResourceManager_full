// sync_demo.h - deterministic demonstrations of process synchronization.
//
// These are SIMULATIONS driven by a fixed scheduling rule, not real OS threads, so every run produces the
// same trace (repeatable for a lab report and for tests). A tiny "virtual machine" runs the threads:
//   * each ROUND, every runnable thread executes exactly ONE instruction, in thread-index order;
//   * WAIT on a semaphore with value 0 blocks the thread (it joins that semaphore's FIFO queue);
//   * SIGNAL wakes the first waiter (it takes over the permit) or increments the semaphore;
//   * if every unfinished thread is blocked, the run stops and reports a DEADLOCK.
//
// Scenario 1  raceCondition(): two threads do `counter++` (READ, INC, WRITE). Without a mutex, one thread's
//             READ can happen between the other's READ and WRITE, so updates are LOST. With a mutex
//             (a binary semaphore) the critical section is exclusive and no update is lost.
// Scenario 2  producerConsumer(): bounded buffer with semaphores empty / full / mutex. `wrongOrder`
//             swaps the producer's two WAITs, which can deadlock - a classic mistake.
//
// (The real, concurrent code in the project - the database connection pool and login limiter in the Python
// backend - uses real locks. This module only demonstrates the concepts.)
#pragma once

#include <string>
#include <vector>

struct SyncStep {
    int step = 0;                 // 1-based instruction counter
    int round = 0;
    int thread = 0;
    std::string threadName;
    std::string action;           // e.g. "READ", "WAIT(mutex)", "BLOCKED(mutex)"
    std::string detail;
    int counter = 0;              // shared counter (race scenario)
    std::vector<int> buffer;      // bounded buffer contents (producer/consumer)
    std::vector<int> semaphores;  // value of each semaphore after the step
};

struct SyncResult {
    bool ok = true;
    std::vector<std::string> errors;
    std::string scenario;
    std::vector<std::string> semaphoreNames;
    std::vector<SyncStep> steps;
    bool deadlock = false;
    bool truncated = false;       // stopped by the safety limit
    // race scenario
    int expectedCounter = 0;
    int finalCounter = 0;
    int lostUpdates = 0;
    // producer / consumer scenario
    int produced = 0;
    int consumed = 0;
    int maxBufferOccupancy = 0;
    bool consumedInOrder = true;
    std::string summary;
};

SyncResult raceCondition(int incrementsPerThread, bool useMutex);
// consumerDelay = number of rounds the consumer thread starts late. With the lock-step schedule alone the
// consumer keeps the buffer nearly empty and the wrong order never deadlocks; a delay (>= 5 * bufferSize + 2)
// lets the producer fill the buffer first, which exposes the deadlock.
SyncResult producerConsumer(int items, int bufferSize, bool wrongOrder, int consumerDelay = 0);
