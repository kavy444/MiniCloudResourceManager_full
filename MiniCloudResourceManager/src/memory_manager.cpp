#include "memory_manager.h"

#include <algorithm>

std::string fitStrategyName(FitStrategy s) {
    switch (s) {
        case FitStrategy::FIRST_FIT: return "FIRST_FIT";
        case FitStrategy::BEST_FIT: return "BEST_FIT";
        case FitStrategy::WORST_FIT: return "WORST_FIT";
    }
    return "FIRST_FIT";
}

bool parseFitStrategy(const std::string& text, FitStrategy& out) {
    for (FitStrategy s : {FitStrategy::FIRST_FIT, FitStrategy::BEST_FIT, FitStrategy::WORST_FIT}) {
        if (fitStrategyName(s) == text) {
            out = s;
            return true;
        }
    }
    return false;
}

MemoryStats computeMemoryStats(const std::vector<MemoryBlock>& blocks) {
    MemoryStats st;
    long requested = 0;
    for (const MemoryBlock& b : blocks) {
        st.total += b.size;
        if (b.free) {
            st.free += b.size;
            ++st.freeBlocks;
            st.largestFree = std::max(st.largestFree, b.size);
        } else {
            st.allocated += b.size;
            st.internalFragmentation += b.size - b.requested;
            requested += b.requested;
        }
    }
    st.externalFragmentation = st.free - st.largestFree;
    if (st.total > 0) st.utilizationPercent = 100.0 * static_cast<double>(requested) / st.total;
    return st;
}

namespace {
constexpr int kMaxMemory = 1000000000;
constexpr std::size_t kMaxOps = 1000;
constexpr std::size_t kMaxPartitions = 64;
}  // namespace

std::vector<std::string> validateMemoryRequest(const MemoryConfig& config, const std::vector<MemoryOp>& ops) {
    std::vector<std::string> errors;
    if (config.partitions.empty()) {
        if (config.totalSize < 1 || config.totalSize > kMaxMemory) {
            errors.push_back("total memory size must be between 1 and " + std::to_string(kMaxMemory));
        }
    } else {
        if (config.partitions.size() > kMaxPartitions) errors.push_back("too many partitions (maximum 64)");
        long long sum = 0;
        for (int p : config.partitions) {
            if (p < 1 || p > kMaxMemory) errors.push_back("every partition must be between 1 and " + std::to_string(kMaxMemory));
            sum += p;
        }
        if (sum > kMaxMemory) errors.push_back("partitions add up to more than the maximum memory size");
    }
    if (ops.empty()) errors.push_back("at least one operation is required");
    if (ops.size() > kMaxOps) errors.push_back("too many operations (maximum 1000)");
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const MemoryOp& op = ops[i];
        const std::string who = "operation " + std::to_string(i + 1) + ": ";
        if (op.type != "ALLOC" && op.type != "FREE") errors.push_back(who + "type must be ALLOC or FREE");
        if (op.process.empty() || op.process.size() > 50) errors.push_back(who + "process name must be 1-50 characters");
        if (op.type == "ALLOC" && (op.size < 1 || op.size > kMaxMemory)) {
            errors.push_back(who + "size must be between 1 and " + std::to_string(kMaxMemory));
        }
    }
    return errors;
}

namespace {

// Index of the block the strategy picks for `request`, or -1 when no free block is large enough.
int chooseBlock(const std::vector<MemoryBlock>& blocks, int request, FitStrategy strategy) {
    int chosen = -1;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const MemoryBlock& b = blocks[i];
        if (!b.free || b.size < request) continue;
        if (chosen < 0) {
            chosen = static_cast<int>(i);
            if (strategy == FitStrategy::FIRST_FIT) break;
            continue;
        }
        const int cur = blocks[static_cast<std::size_t>(chosen)].size;
        // strict comparisons: an equal block never replaces the earlier (lower-address) one
        if (strategy == FitStrategy::BEST_FIT && b.size < cur) chosen = static_cast<int>(i);
        if (strategy == FitStrategy::WORST_FIT && b.size > cur) chosen = static_cast<int>(i);
    }
    return chosen;
}

void coalesce(std::vector<MemoryBlock>& blocks) {
    for (std::size_t i = 0; i + 1 < blocks.size();) {
        if (blocks[i].free && blocks[i + 1].free) {
            blocks[i].size += blocks[i + 1].size;
            blocks.erase(blocks.begin() + static_cast<long>(i) + 1);
        } else {
            ++i;
        }
    }
}

}  // namespace

MemoryResult simulateMemory(const MemoryConfig& config, const std::vector<MemoryOp>& ops) {
    MemoryResult result;
    result.strategy = config.strategy;
    result.fixedPartitions = !config.partitions.empty();
    result.errors = validateMemoryRequest(config, ops);
    if (!result.errors.empty()) {
        result.ok = false;
        return result;
    }

    std::vector<MemoryBlock> blocks;
    if (result.fixedPartitions) {
        int addr = 0;
        for (int size : config.partitions) {
            MemoryBlock b;
            b.start = addr;
            b.size = size;
            blocks.push_back(b);
            addr += size;
        }
    } else {
        MemoryBlock whole;
        whole.size = config.totalSize;
        blocks.push_back(whole);
    }

    int index = 0;
    for (const MemoryOp& op : ops) {
        MemoryStep step;
        step.index = ++index;
        step.op = op;

        if (op.type == "ALLOC") {
            const bool exists = std::any_of(blocks.begin(), blocks.end(), [&](const MemoryBlock& b) {
                return !b.free && b.owner == op.process;
            });
            if (exists) {
                step.message = "Process " + op.process + " already holds memory; free it first";
            } else {
                const int pick = chooseBlock(blocks, op.size, config.strategy);
                if (pick < 0) {
                    const MemoryStats now = computeMemoryStats(blocks);
                    if (now.free >= op.size) {
                        step.message = "Allocation of " + std::to_string(op.size) + " for " + op.process +
                                       " FAILED: " + std::to_string(now.free) + " free in total but the largest block is only " +
                                       std::to_string(now.largestFree) + " (external fragmentation)";
                    } else {
                        step.message = "Allocation of " + std::to_string(op.size) + " for " + op.process +
                                       " FAILED: only " + std::to_string(now.free) + " free in total";
                    }
                    ++result.failedAllocations;
                } else {
                    MemoryBlock& chosen = blocks[static_cast<std::size_t>(pick)];
                    step.success = true;
                    step.blockStart = chosen.start;
                    const int blockSize = chosen.size;
                    const int start = chosen.start;
                    chosen.free = false;
                    chosen.owner = op.process;
                    chosen.requested = op.size;
                    if (!result.fixedPartitions && blockSize > op.size) {
                        chosen.size = op.size;
                        MemoryBlock rest;
                        rest.start = start + op.size;
                        rest.size = blockSize - op.size;
                        blocks.insert(blocks.begin() + pick + 1, rest);
                    }
                    step.message = "Allocated " + std::to_string(op.size) + " to " + op.process + " at address " +
                                   std::to_string(start) + " using " + fitStrategyName(config.strategy) +
                                   " (chose the block of size " + std::to_string(blockSize) + ")";
                    if (result.fixedPartitions && blockSize > op.size) {
                        step.message += "; internal fragmentation " + std::to_string(blockSize - op.size);
                    }
                }
            }
        } else {  // FREE
            auto it = std::find_if(blocks.begin(), blocks.end(), [&](const MemoryBlock& b) {
                return !b.free && b.owner == op.process;
            });
            if (it == blocks.end()) {
                step.message = "Cannot free " + op.process + ": it holds no memory";
            } else {
                step.success = true;
                step.blockStart = it->start;
                step.message = "Freed " + std::to_string(it->size) + " from " + op.process + " at address " + std::to_string(it->start);
                it->free = true;
                it->owner.clear();
                it->requested = 0;
                if (!result.fixedPartitions) coalesce(blocks);
            }
        }
        step.blocks = blocks;
        step.stats = computeMemoryStats(blocks);
        result.steps.push_back(step);
    }
    result.finalStats = computeMemoryStats(blocks);
    return result;
}
