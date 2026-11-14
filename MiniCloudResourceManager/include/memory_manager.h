// memory_manager.h - contiguous memory allocation: First Fit, Best Fit, Worst Fit.
//
// Two models share one implementation:
//   * DYNAMIC partitions (config.partitions empty): memory starts as ONE free hole of `totalSize`.
//     Allocation splits the chosen hole; freeing merges adjacent holes (coalescing).
//   * FIXED partitions (config.partitions non-empty): memory is the given list of blocks. A block is never
//     split or merged, so a process wastes (block - request) = INTERNAL fragmentation.
//
// Strategy (all ties go to the LOWEST ADDRESS, so results are deterministic):
//   FIRST_FIT : the lowest-address free block that is large enough
//   BEST_FIT  : the smallest free block that is large enough
//   WORST_FIT : the largest free block
//
// Units are arbitrary (the UI uses KB). This simulates the algorithms; it does not manage real memory.
#pragma once

#include <string>
#include <vector>

enum class FitStrategy { FIRST_FIT, BEST_FIT, WORST_FIT };

std::string fitStrategyName(FitStrategy s);
bool parseFitStrategy(const std::string& text, FitStrategy& out);

struct MemoryBlock {
    int start = 0;
    int size = 0;
    bool free = true;
    std::string owner;       // process name when allocated
    int requested = 0;       // bytes the owner asked for (size - requested = internal fragmentation)
};

struct MemoryOp {
    std::string type;        // "ALLOC" or "FREE"
    std::string process;
    int size = 0;            // ALLOC only
};

struct MemoryStats {
    int total = 0;
    int allocated = 0;               // sum of block sizes owned by processes
    int free = 0;
    int largestFree = 0;
    int freeBlocks = 0;
    int externalFragmentation = 0;   // free memory that cannot be used for ONE request: free - largestFree
    int internalFragmentation = 0;   // sum(block.size - requested) over allocated blocks
    double utilizationPercent = 0;   // requested bytes / total
};

struct MemoryConfig {
    FitStrategy strategy = FitStrategy::FIRST_FIT;
    int totalSize = 0;               // dynamic mode
    std::vector<int> partitions;     // fixed mode (overrides totalSize)
};

struct MemoryStep {
    int index = 0;
    MemoryOp op;
    bool success = false;
    std::string message;
    int blockStart = -1;             // where the process was placed (ALLOC success)
    std::vector<MemoryBlock> blocks; // memory map AFTER the operation
    MemoryStats stats;
};

struct MemoryResult {
    bool ok = true;
    std::vector<std::string> errors; // validation problems (nothing was simulated)
    FitStrategy strategy = FitStrategy::FIRST_FIT;
    bool fixedPartitions = false;
    std::vector<MemoryStep> steps;
    int failedAllocations = 0;
    MemoryStats finalStats;
};

MemoryStats computeMemoryStats(const std::vector<MemoryBlock>& blocks);

std::vector<std::string> validateMemoryRequest(const MemoryConfig& config, const std::vector<MemoryOp>& ops);

MemoryResult simulateMemory(const MemoryConfig& config, const std::vector<MemoryOp>& ops);
