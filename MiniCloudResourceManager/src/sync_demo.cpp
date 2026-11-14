#include "sync_demo.h"

#include <algorithm>
#include <deque>

namespace {

enum class Op { WAIT, SIGNAL, READ, INC, WRITE, PUT, GET };

struct Instr {
    Op op;
    int sem = -1;
};

struct Semaphore {
    std::string name;
    int value = 0;
    std::deque<int> waiters;
};

struct Thread {
    std::string name;
    std::vector<Instr> program;
    std::size_t pc = 0;
    bool blocked = false;
    int reg = 0;
    int startRound = 1;  // the thread executes nothing before this round (models a late-starting thread)
};

constexpr int kMaxSteps = 20000;

class Machine {
public:
    std::vector<Semaphore> sems;
    std::vector<Thread> threads;
    int counter = 0;
    std::deque<int> buffer;
    int produced = 0;
    std::vector<int> consumedItems;
    int maxOccupancy = 0;
    SyncResult result;

    void run() {
        int step = 0;
        int round = 0;
        while (true) {
            bool anyUnfinished = false;
            bool anyRunnable = false;
            for (const Thread& t : threads) {
                if (t.pc < t.program.size() || t.blocked) anyUnfinished = true;
                if (t.pc < t.program.size() && !t.blocked) anyRunnable = true;
            }
            if (!anyUnfinished) break;
            if (!anyRunnable) {
                result.deadlock = true;
                break;
            }
            ++round;
            for (std::size_t ti = 0; ti < threads.size(); ++ti) {
                Thread& t = threads[ti];
                if (t.blocked || t.pc >= t.program.size() || round < t.startRound) continue;
                if (step >= kMaxSteps) {
                    result.truncated = true;
                    return;
                }
                execute(ti, ++step, round);
            }
        }
    }

private:
    SyncStep base(std::size_t ti, int step, int round) const {
        SyncStep s;
        s.step = step;
        s.round = round;
        s.thread = static_cast<int>(ti);
        s.threadName = threads[ti].name;
        return s;
    }

    void snapshot(SyncStep& s) const {
        s.counter = counter;
        s.buffer.assign(buffer.begin(), buffer.end());
        for (const Semaphore& m : sems) s.semaphores.push_back(m.value);
    }

    void execute(std::size_t ti, int step, int round) {
        Thread& t = threads[ti];
        const Instr ins = t.program[t.pc];
        SyncStep s = base(ti, step, round);
        switch (ins.op) {
            case Op::WAIT: {
                Semaphore& sem = sems[static_cast<std::size_t>(ins.sem)];
                if (sem.value > 0) {
                    --sem.value;
                    ++t.pc;
                    s.action = "WAIT(" + sem.name + ")";
                    s.detail = "acquired " + sem.name;
                } else {
                    sem.waiters.push_back(static_cast<int>(ti));
                    t.blocked = true;  // pc stays; the WAIT completes when a SIGNAL hands over the permit
                    s.action = "BLOCKED(" + sem.name + ")";
                    s.detail = sem.name + " is 0, so " + t.name + " sleeps in its queue";
                }
                break;
            }
            case Op::SIGNAL: {
                Semaphore& sem = sems[static_cast<std::size_t>(ins.sem)];
                ++t.pc;
                s.action = "SIGNAL(" + sem.name + ")";
                if (!sem.waiters.empty()) {
                    const int woken = sem.waiters.front();
                    sem.waiters.pop_front();
                    Thread& w = threads[static_cast<std::size_t>(woken)];
                    w.blocked = false;
                    ++w.pc;  // its WAIT is now complete (it owns the permit)
                    s.detail = "woke " + w.name + ", which now holds " + sem.name;
                } else {
                    ++sem.value;
                    s.detail = sem.name + " becomes " + std::to_string(sem.value);
                }
                break;
            }
            case Op::READ:
                t.reg = counter;
                ++t.pc;
                s.action = "READ";
                s.detail = t.name + " reads counter = " + std::to_string(counter);
                break;
            case Op::INC:
                ++t.reg;
                ++t.pc;
                s.action = "INC";
                s.detail = t.name + " computes " + std::to_string(t.reg) + " in its register";
                break;
            case Op::WRITE:
                counter = t.reg;
                ++t.pc;
                s.action = "WRITE";
                s.detail = t.name + " writes counter = " + std::to_string(counter);
                break;
            case Op::PUT:
                buffer.push_back(++produced);
                maxOccupancy = std::max(maxOccupancy, static_cast<int>(buffer.size()));
                ++t.pc;
                s.action = "PUT";
                s.detail = "item " + std::to_string(produced) + " added to the buffer";
                break;
            case Op::GET: {
                const int item = buffer.front();
                buffer.pop_front();
                consumedItems.push_back(item);
                ++t.pc;
                s.action = "GET";
                s.detail = "item " + std::to_string(item) + " removed from the buffer";
                break;
            }
        }
        snapshot(s);
        result.steps.push_back(s);
    }
};

}  // namespace

SyncResult raceCondition(int n, bool useMutex) {
    SyncResult r;
    r.scenario = useMutex ? "RACE_WITH_MUTEX" : "RACE_WITHOUT_MUTEX";
    if (n < 1 || n > 50) {
        r.ok = false;
        r.errors.push_back("increments per thread must be between 1 and 50");
        return r;
    }
    Machine m;
    m.sems.push_back(Semaphore{"mutex", 1, {}});
    for (int i = 0; i < 2; ++i) {
        Thread t;
        t.name = "T" + std::to_string(i + 1);
        for (int k = 0; k < n; ++k) {
            if (useMutex) t.program.push_back({Op::WAIT, 0});
            t.program.push_back({Op::READ, -1});
            t.program.push_back({Op::INC, -1});
            t.program.push_back({Op::WRITE, -1});
            if (useMutex) t.program.push_back({Op::SIGNAL, 0});
        }
        m.threads.push_back(t);
    }
    m.run();
    m.result.scenario = r.scenario;
    m.result.semaphoreNames = {"mutex"};
    m.result.expectedCounter = 2 * n;
    m.result.finalCounter = m.counter;
    m.result.lostUpdates = 2 * n - m.counter;
    m.result.summary = useMutex
                           ? "With the mutex only one thread is in the critical section at a time: final counter " +
                                 std::to_string(m.counter) + " = expected " + std::to_string(2 * n) + "."
                           : "Without a mutex the threads interleave READ/INC/WRITE: final counter " + std::to_string(m.counter) +
                                 " instead of " + std::to_string(2 * n) + " (" + std::to_string(2 * n - m.counter) + " updates lost).";
    return m.result;
}

SyncResult producerConsumer(int items, int bufferSize, bool wrongOrder, int consumerDelay) {
    SyncResult r;
    r.scenario = wrongOrder ? "PRODUCER_CONSUMER_WRONG_ORDER" : "PRODUCER_CONSUMER";
    if (items < 1 || items > 30) r.errors.push_back("items must be between 1 and 30");
    if (bufferSize < 1 || bufferSize > 10) r.errors.push_back("buffer size must be between 1 and 10");
    if (consumerDelay < 0 || consumerDelay > 200) r.errors.push_back("consumer delay must be between 0 and 200 rounds");
    if (!r.errors.empty()) {
        r.ok = false;
        return r;
    }
    Machine m;
    m.sems.push_back(Semaphore{"empty", bufferSize, {}});  // 0
    m.sems.push_back(Semaphore{"full", 0, {}});            // 1
    m.sems.push_back(Semaphore{"mutex", 1, {}});           // 2
    Thread producer;
    producer.name = "Producer";
    Thread consumer;
    consumer.name = "Consumer";
    for (int i = 0; i < items; ++i) {
        if (wrongOrder) {
            producer.program.push_back({Op::WAIT, 2});  // takes the lock BEFORE checking for space: can deadlock
            producer.program.push_back({Op::WAIT, 0});
        } else {
            producer.program.push_back({Op::WAIT, 0});
            producer.program.push_back({Op::WAIT, 2});
        }
        producer.program.push_back({Op::PUT, -1});
        producer.program.push_back({Op::SIGNAL, 2});
        producer.program.push_back({Op::SIGNAL, 1});

        consumer.program.push_back({Op::WAIT, 1});
        consumer.program.push_back({Op::WAIT, 2});
        consumer.program.push_back({Op::GET, -1});
        consumer.program.push_back({Op::SIGNAL, 2});
        consumer.program.push_back({Op::SIGNAL, 0});
    }
    consumer.startRound = 1 + consumerDelay;  // a late consumer lets the producer fill the buffer first
    m.threads.push_back(producer);
    m.threads.push_back(consumer);
    m.run();
    SyncResult out = m.result;
    out.scenario = r.scenario;
    out.semaphoreNames = {"empty", "full", "mutex"};
    out.produced = m.produced;
    out.consumed = static_cast<int>(m.consumedItems.size());
    out.maxBufferOccupancy = m.maxOccupancy;
    for (std::size_t i = 0; i < m.consumedItems.size(); ++i) {
        if (m.consumedItems[i] != static_cast<int>(i) + 1) out.consumedInOrder = false;
    }
    if (out.deadlock) {
        out.summary = "DEADLOCK: the buffer is full, the producer holds the mutex while waiting for an empty slot, and the consumer "
                      "needs that mutex to free one. Always WAIT on the counting semaphore before the mutex.";
    } else {
        out.summary = "Produced " + std::to_string(out.produced) + ", consumed " + std::to_string(out.consumed) +
                      " items in order; the buffer never held more than " + std::to_string(out.maxBufferOccupancy) +
                      " (capacity " + std::to_string(bufferSize) + ").";
    }
    return out;
}
