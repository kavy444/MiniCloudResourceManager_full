#include <iostream>

#include "test_framework.h"

int main() {
    int failedTests = 0;
    for (const tf::Case& c : tf::registry()) {
        const int before = tf::failures();
        c.fn();
        const bool ok = tf::failures() == before;
        if (!ok) ++failedTests;
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << c.name << "\n";
    }
    std::cout << "\n" << tf::registry().size() << " tests, " << tf::checks() << " checks, " << tf::failures()
              << " failed checks, " << failedTests << " failed tests\n";
    return tf::failures() == 0 ? 0 : 1;
}
