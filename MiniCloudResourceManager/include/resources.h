// resources.h - a plain bundle of the three resource types we simulate.
#pragma once

// Units: CPU in cores, RAM in GB, storage in GB. Signed ints on purpose, so
// that invalid (zero/negative) user input can be represented and rejected.
struct Resources {
    int cpu = 0;
    int ramGb = 0;
    int storageGb = 0;
};
