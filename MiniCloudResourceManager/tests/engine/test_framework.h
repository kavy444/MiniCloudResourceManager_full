// Minimal self-registering test framework (no external library).
#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace tf {

struct Case {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}
inline int& checks() { static int n = 0; return n; }
inline int& failures() { static int n = 0; return n; }

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

}  // namespace tf

#define TEST(name)                                           \
    static void name();                                      \
    static tf::Registrar registrar_##name(#name, name);      \
    static void name()

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        ++tf::checks();                                                                       \
        if (!(cond)) {                                                                        \
            ++tf::failures();                                                                 \
            std::cout << "    FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";    \
        }                                                                                     \
    } while (0)

#define CHECK_NEAR(a, b)                                                                      \
    do {                                                                                      \
        ++tf::checks();                                                                       \
        if (!tf::near((a), (b))) {                                                            \
            ++tf::failures();                                                                 \
            std::cout << "    FAIL " << __FILE__ << ":" << __LINE__ << "  " #a " = " << (a)   \
                      << ", expected " << (b) << "\n";                                        \
        }                                                                                     \
    } while (0)
